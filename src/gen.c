/*
 * gen.c — type checking, agent resolution and LLVM IR generation.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 *
 * One pass over each function both checks types and emits textual LLVM IR.
 * Before that, a declaration pass collects structs, agents (merging
 * `extends` chains) and function signatures, so code can refer to anything
 * declared anywhere in the program.
 *
 * Runtime ABI: see runtime/hl_rt.h. Descriptor layouts there and the
 * %hl.* types emitted here must stay in step.
 */
#include "gen.h"
#include "strbuf.h"
#include "cJSON.h"
#include "chacha20.h"

#include <ctype.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* types                                                              */
/* ================================================================== */

typedef enum { T_VOID, T_INT, T_FLOAT, T_BOOL, T_STRING, T_ARRAY, T_STRUCT, T_AGENT } tkind;

typedef struct type type;
typedef struct sdef sdef;
typedef struct adef adef;
typedef struct fsig fsig;

struct type {
    tkind k;
    type *elem;     /* arrays */
    sdef *sd;       /* structs */
    adef *ad;       /* agent names used as values (Agent.ask) */
};

typedef struct {
    char *name;
    type *t;
    char *desc;
    int64_t off;
} fdef;

struct sdef {
    char *name;         /* Foo, Agent.Input, Agent.f.params */
    fdef *f;
    int nf, capf;
    int64_t size;
    adef *input_of;     /* the agent this is the input of (enables .run()) */
    node *decl;
    char *desc_sym;     /* @__ty.<name> once emitted */
};

struct fsig {
    char *name;
    char *ll;           /* @hl_fn_x or @hl_tool_A_x */
    type **pt;
    char **pn;
    int np;
    type *ret;
    node *decl;
    adef *tool_of;      /* tools: the agent that declared it */
    char *doc;
    char *wrapper;      /* tools: @hl_toolw_A_x */
    sdef *params_sd;    /* tools: struct of the parameters */
    char *binding;      /* declarative tools: sh http print read write agent */
    char *templ, *templ2;
    adef *sub;          /* binding agent */
};

typedef struct {
    char **v;
    int n, cap;
} strvec;

typedef struct {
    adef *owner;
    char *text;
} pseg;

/* Agents and skills share this record. A skill is instructions plus the
 * tools (Hola tools and tool functions) that come with them; an agent that
 * lists it in `skills` gets all of it. */
struct adef {
    char *name, *doc;
    int is_abstract;
    int is_skill;
    char *instructions;     /* skills */
    node *decl;
    int state;          /* 0 new, 1 resolving, 2 done */
    char *profile, *goal, *task;
    int64_t max_turns;
    pseg *persona;
    int npersona, cappersona;
    strvec skills, tools, deny, parents;
    sdef *input, *output;
    int implicit_input;     /* input is only the default `prompt` field */
    fsig **ftools;
    int nftools, capftools;
    char *sym;
};

static type ty_void = {.k = T_VOID}, ty_int = {.k = T_INT}, ty_float = {.k = T_FLOAT},
            ty_bool = {.k = T_BOOL}, ty_string = {.k = T_STRING};

static type **g_arrays;
static int g_narrays;

static type *array_of(type *e) {
    for (int i = 0; i < g_narrays; i++)
        if (g_arrays[i]->elem == e) return g_arrays[i];
    type *t = xcalloc(1, sizeof(*t));
    t->k = T_ARRAY;
    t->elem = e;
    g_arrays = realloc(g_arrays, sizeof(*g_arrays) * (size_t)(g_narrays + 1));
    g_arrays[g_narrays++] = t;
    return t;
}

static type *struct_type(sdef *sd) {
    /* one type object per struct, kept on the struct itself */
    static sdef **keys;
    static type **vals;
    static int n;
    for (int i = 0; i < n; i++)
        if (keys[i] == sd) return vals[i];
    type *t = xcalloc(1, sizeof(*t));
    t->k = T_STRUCT;
    t->sd = sd;
    keys = realloc(keys, sizeof(*keys) * (size_t)(n + 1));
    vals = realloc(vals, sizeof(*vals) * (size_t)(n + 1));
    keys[n] = sd;
    vals[n++] = t;
    return t;
}

static type *agent_type(adef *ad) {
    type *t = xcalloc(1, sizeof(*t));
    t->k = T_AGENT;
    t->ad = ad;
    return t;
}

static char *type_name(type *t) {
    switch (t->k) {
    case T_VOID: return xstrdup("void");
    case T_INT: return xstrdup("int");
    case T_FLOAT: return xstrdup("float");
    case T_BOOL: return xstrdup("bool");
    case T_STRING: return xstrdup("string");
    case T_ARRAY: { char *e = type_name(t->elem); char *s = xasprintf("[]%s", e); free(e); return s; }
    case T_STRUCT: return xstrdup(t->sd->name);
    case T_AGENT: return xasprintf("agent %s", t->ad->name);
    }
    return xstrdup("?");
}

static const char *lt(type *t) {
    switch (t->k) {
    case T_VOID: return "void";
    case T_INT: return "i64";
    case T_FLOAT: return "double";
    case T_BOOL: return "i1";
    default: return "ptr";
    }
}

static int64_t slot_size(type *t) { return t->k == T_BOOL ? 1 : 8; }

/* ================================================================== */
/* diagnostics                                                        */
/* ================================================================== */

static int g_errors;

static void err(node *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void err(node *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", n && n->file ? n->file : "?", n ? n->line : 0);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    g_errors++;
}

/* ================================================================== */
/* program tables                                                     */
/* ================================================================== */

static sdef **g_structs;
static int g_nstructs;
static fsig **g_funcs;
static int g_nfuncs;
static adef **g_agents;
static int g_nagents;

static sdef *find_struct(const char *name) {
    for (int i = 0; i < g_nstructs; i++)
        if (strcmp(g_structs[i]->name, name) == 0) return g_structs[i];
    return NULL;
}

static fsig *find_func(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (!g_funcs[i]->tool_of && strcmp(g_funcs[i]->name, name) == 0) return g_funcs[i];
    return NULL;
}

static adef *find_agent(const char *name) {
    for (int i = 0; i < g_nagents; i++)
        if (strcmp(g_agents[i]->name, name) == 0) return g_agents[i];
    return NULL;
}

static sdef *new_struct(const char *name, node *decl) {
    sdef *sd = xcalloc(1, sizeof(*sd));
    sd->name = xstrdup(name);
    sd->decl = decl;
    g_structs = realloc(g_structs, sizeof(*g_structs) * (size_t)(g_nstructs + 1));
    g_structs[g_nstructs++] = sd;
    return sd;
}

static int struct_field(sdef *sd, const char *name) {
    for (int i = 0; i < sd->nf; i++)
        if (strcmp(sd->f[i].name, name) == 0) return i;
    return -1;
}

static void add_field(sdef *sd, const char *name, type *t, const char *desc) {
    if (sd->nf == sd->capf) {
        sd->capf = sd->capf ? sd->capf * 2 : 8;
        sd->f = realloc(sd->f, sizeof(*sd->f) * (size_t)sd->capf);
    }
    fdef *f = &sd->f[sd->nf++];
    memset(f, 0, sizeof(*f));
    f->name = xstrdup(name);
    f->t = t;
    f->desc = desc ? xstrdup(desc) : NULL;
}

/* Natural alignment, as LLVM lays out { i64, i1, ptr, ... } on x86_64. */
static void layout(sdef *sd) {
    int64_t off = 0, align = 1;
    for (int i = 0; i < sd->nf; i++) {
        int64_t s = slot_size(sd->f[i].t);
        off = (off + s - 1) / s * s;
        sd->f[i].off = off;
        off += s;
        if (s > align) align = s;
    }
    sd->size = off ? (off + align - 1) / align * align : 1;
}

static void sv_add(strvec *v, const char *s) {
    for (int i = 0; i < v->n; i++)
        if (strcmp(v->v[i], s) == 0) return;
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->v = realloc(v->v, sizeof(*v->v) * (size_t)v->cap);
    }
    v->v[v->n++] = xstrdup(s);
}

static void resolve_agent(adef *a);

static type *resolve_type(node *tn) {
    if (!tn) return &ty_void;
    if (tn->kind == N_TARRAY) return array_of(resolve_type(tn->type));
    const char *n = tn->name;
    if (strcmp(n, "int") == 0) return &ty_int;
    if (strcmp(n, "float") == 0) return &ty_float;
    if (strcmp(n, "bool") == 0) return &ty_bool;
    if (strcmp(n, "string") == 0) return &ty_string;
    const char *dot = strchr(n, '.');
    if (dot) {
        char *an = strndup(n, (size_t)(dot - n));
        adef *a = find_agent(an);
        free(an);
        if (!a) { err(tn, "unknown agent in type %s", n); return &ty_string; }
        resolve_agent(a);
        if (strcmp(dot + 1, "Input") == 0 && a->input) return struct_type(a->input);
        if (strcmp(dot + 1, "Output") == 0 && a->output) return struct_type(a->output);
        err(tn, "agent %s has no type %s (use Input, or Output when it declares one)", a->name, dot + 1);
        return &ty_string;
    }
    sdef *sd = find_struct(n);
    if (sd) return struct_type(sd);
    if (find_agent(n)) {
        err(tn, "%s is an agent; its types are %s.Input and %s.Output", n, n, n);
        return &ty_string;
    }
    err(tn, "unknown type %s", n);
    return &ty_string;
}

/* ================================================================== */
/* agents: resolution and `extends` merging                           */
/* ================================================================== */

static char *read_text_file(node *at, const char *rel) {
    char *path;
    if (rel[0] == '/') path = xstrdup(rel);
    else {
        char *f = xstrdup(at->file ? at->file : ".");
        path = xasprintf("%s/%s", dirname(f), rel);
        free(f);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) { err(at, "cannot read %s", path); free(path); return xstrdup(""); }
    strbuf b = {0};
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) sb_putn(&b, buf, n);
    fclose(fp);
    free(path);
    return sb_take(&b);
}

/* A string-valued agent item: "text" or file("path"). */
static char *item_text(node *it) {
    node *v = it->a;
    if (v->kind == N_STR) return xstrdup(v->text);
    if (v->kind == N_CALL && strcmp(v->name, "file") == 0) return read_text_file(v, v->text);
    err(it, "%s takes a string or file(\"path\")", it->name);
    return xstrdup("");
}

static void add_persona(adef *a, adef *owner, const char *text) {
    for (int i = 0; i < a->npersona; i++)
        if (a->persona[i].owner == owner) return;   /* diamond: once */
    if (a->npersona == a->cappersona) {
        a->cappersona = a->cappersona ? a->cappersona * 2 : 4;
        a->persona = realloc(a->persona, sizeof(*a->persona) * (size_t)a->cappersona);
    }
    a->persona[a->npersona].owner = owner;
    a->persona[a->npersona++].text = xstrdup(text);
}

static void add_tool(adef *a, fsig *f) {
    for (int i = 0; i < a->nftools; i++)
        if (strcmp(a->ftools[i]->name, f->name) == 0) { a->ftools[i] = f; return; }
    if (a->nftools == a->capftools) {
        a->capftools = a->capftools ? a->capftools * 2 : 4;
        a->ftools = realloc(a->ftools, sizeof(*a->ftools) * (size_t)a->capftools);
    }
    a->ftools[a->nftools++] = f;
}

static fsig *declare_func(node *fn, adef *tool_of);
static fsig *declare_bound_tool(node *tn);
static fsig *find_bound_tool(const char *name);

static void merge_fields(adef *a, sdef *dst, sdef *src, node *at) {
    for (int i = 0; i < src->nf; i++) {
        int k = struct_field(dst, src->f[i].name);
        if (k < 0) add_field(dst, src->f[i].name, src->f[i].t, src->f[i].desc);
        else if (dst->f[k].t != src->f[i].t) {
            char *x = type_name(dst->f[k].t), *y = type_name(src->f[i].t);
            err(at, "agent %s: input field %s is %s in one parent and %s in another", a->name,
                src->f[i].name, x, y);
            free(x); free(y);
        }
    }
}

static void resolve_agent(adef *a) {
    if (a->state == 2) return;
    if (a->state == 1) { err(a->decl, "agent %s extends itself (cycle)", a->name); return; }
    a->state = 1;
    node *d = a->decl;

    /* ---- inherit, left to right ---- */
    sdef *in = new_struct(xasprintf("%s.Input", a->name), d);
    sdef *parent_out = NULL;
    int out_conflict = 0;
    char *parent_task = NULL;
    int task_conflict = 0;
    for (size_t i = 0; i < d->list2.len; i++) {
        node *pn = d->list2.items[i];
        adef *p = find_agent(pn->name);
        if (!p) { err(pn, "agent %s extends unknown agent %s", a->name, pn->name); continue; }
        if (p == a) { err(pn, "agent %s extends itself", a->name); continue; }
        if (p->is_skill) { err(pn, "%s is a skill: list it in skills [...] instead of extends", p->name); continue; }
        resolve_agent(p);
        if (p->state != 2) continue;
        sv_add(&a->parents, p->name);
        for (int k = 0; k < p->npersona; k++) add_persona(a, p->persona[k].owner, p->persona[k].text);
        for (int k = 0; k < p->skills.n; k++) sv_add(&a->skills, p->skills.v[k]);
        for (int k = 0; k < p->tools.n; k++) sv_add(&a->tools, p->tools.v[k]);
        for (int k = 0; k < p->deny.n; k++) sv_add(&a->deny, p->deny.v[k]);
        for (int k = 0; k < p->nftools; k++) add_tool(a, p->ftools[k]);
        if (p->profile) a->profile = p->profile;
        if (p->goal) a->goal = p->goal;
        if (p->max_turns) a->max_turns = p->max_turns;
        if (p->input && !p->implicit_input) merge_fields(a, in, p->input, pn);
        if (p->output) {
            if (parent_out && parent_out != p->output) out_conflict = 1;
            parent_out = p->output;
        }
        if (p->task) {
            if (parent_task && strcmp(parent_task, p->task) != 0) task_conflict = 1;
            parent_task = p->task;
        }
    }

    /* ---- own items ---- */
    node *own_out = NULL, *own_in = NULL;
    int in_override = 0;
    for (size_t i = 0; i < d->list.len; i++) {
        node *it = d->list.items[i];
        const char *k = it->name;
        node *v = it->a;
        if (a->is_skill && strcmp(k, "instructions") && strcmp(k, "tools") && strcmp(k, "deny") &&
            strcmp(k, "func") && strcmp(k, "tool") && strcmp(k, "skills")) {
            err(it, "a skill has instructions, tools, deny, skills, tool and func; '%s' belongs in an agent", k);
            continue;
        }
        if (strcmp(k, "persona") == 0) {
            char *t = item_text(it);
            if (it->flag) a->npersona = 0;
            add_persona(a, a, t);
            free(t);
        } else if (strcmp(k, "profile") == 0 || strcmp(k, "goal") == 0 || strcmp(k, "task") == 0) {
            char *t = item_text(it);
            if (k[0] == 'p') a->profile = t;
            else if (k[0] == 'g') a->goal = t;
            else a->task = t;
        } else if (strcmp(k, "max_turns") == 0) {
            if (v->kind != N_INT || v->ival < 0) err(it, "max_turns takes a non-negative integer");
            else a->max_turns = v->ival;
        } else if (strcmp(k, "skills") == 0) {
            if (v->kind != N_STRLIST) { err(it, "skills takes a list: [my_skill, \"skill-file\"]"); continue; }
            if (it->flag) a->skills.n = 0;
            for (size_t j = 0; j < v->list.len; j++) {
                node *e = v->list.items[j];
                if (e->kind == N_STR) { sv_add(&a->skills, e->text); continue; }   /* a SKILL.md file */
                adef *sk = find_agent(e->name);
                if (!sk || !sk->is_skill) { err(e, "%s is not a skill declared in the program", e->name); continue; }
                if (sk == a) { err(e, "skill %s uses itself", a->name); continue; }
                resolve_agent(sk);
                if (sk->state != 2) continue;
                for (int q = 0; q < sk->skills.n; q++) sv_add(&a->skills, sk->skills.v[q]);
                sv_add(&a->skills, sk->name);
                for (int q = 0; q < sk->tools.n; q++) sv_add(&a->tools, sk->tools.v[q]);
                for (int q = 0; q < sk->deny.n; q++) sv_add(&a->deny, sk->deny.v[q]);
                for (int q = 0; q < sk->nftools; q++) add_tool(a, sk->ftools[q]);
            }
        } else if (strcmp(k, "tools") == 0 || strcmp(k, "deny") == 0) {
            strvec *sv = k[0] == 't' ? &a->tools : &a->deny;
            if (v->kind != N_STRLIST) { err(it, "%s takes a list of strings: [\"a\", \"b\"]", k); continue; }
            if (it->flag) sv->n = 0;
            for (size_t j = 0; j < v->list.len; j++) {
                node *e = v->list.items[j];
                if (e->kind == N_STR) { sv_add(sv, e->text); continue; }      /* a Hola tool or toolset */
                fsig *bt = k[0] == 't' ? find_bound_tool(e->name) : NULL;     /* a tool declared in the program */
                if (!bt) { err(e, "%s: no tool %s is declared in the program (Hola tools are named as strings)", k, e->name); continue; }
                add_tool(a, bt);
            }
        } else if (strcmp(k, "instructions") == 0) {
            if (!a->is_skill) { err(it, "instructions belongs in a skill; an agent has a persona"); continue; }
            a->instructions = item_text(it);
        } else if (strcmp(k, "input") == 0 || strcmp(k, "output") == 0) {
            if (v->kind != N_FIELDS) { err(it, "%s takes a field block: %s { name string }", k, k); continue; }
            if (k[0] == 'i') { own_in = v; in_override = it->flag; }
            else own_out = v;
        } else if (strcmp(k, "tool") == 0) {
            if (it->flag) err(it, "override is not needed for a tool; a tool with the same name replaces the parent's");
            fsig *f = declare_bound_tool(v);
            if (f) add_tool(a, f);
        } else if (strcmp(k, "func") == 0) {
            if (it->flag) err(it, "override is not needed for a tool; a tool with the same name replaces the parent's");
            fsig *f = declare_func(v, a);
            if (f) add_tool(a, f);
        } else {
            err(it, "unknown agent setting '%s' (profile, persona, goal, task, skills, tools, deny, "
                    "max_turns, input, output, func)", k);
        }
    }

    /* ---- input ---- */
    if (own_in) {
        if (in_override) in->nf = 0;
        for (size_t i = 0; i < own_in->list.len; i++) {
            node *f = own_in->list.items[i];
            type *t = resolve_type(f->type);
            int k = struct_field(in, f->name);
            if (k >= 0 && in->f[k].t != t) err(f, "input field %s already has another type from a parent", f->name);
            else if (k >= 0) { if (f->text) in->f[k].desc = xstrdup(f->text); }
            else add_field(in, f->name, t, f->text);
        }
    }
    if (in->nf == 0) {
        add_field(in, "prompt", &ty_string, "The request");
        a->implicit_input = 1;
    }
    layout(in);
    in->input_of = a;
    a->input = in;

    /* ---- output ---- */
    if (own_out) {
        sdef *out = new_struct(xasprintf("%s.Output", a->name), d);
        for (size_t i = 0; i < own_out->list.len; i++) {
            node *f = own_out->list.items[i];
            if (struct_field(out, f->name) >= 0) { err(f, "duplicate output field %s", f->name); continue; }
            add_field(out, f->name, resolve_type(f->type), f->text);
        }
        layout(out);
        a->output = out;
    } else if (out_conflict) {
        err(d, "agent %s: parents declare different outputs; declare this agent's own output", a->name);
    } else {
        a->output = parent_out;
    }

    if (!a->task) {
        if (task_conflict) err(d, "agent %s: parents have different tasks; write this agent's own task", a->name);
        else a->task = parent_task;
    }
    a->state = 2;
}

/* ================================================================== */
/* declarations                                                       */
/* ================================================================== */

static const char *const k_builtins[] = {
    "len", "append", "str", "int", "float", "print", "println", "json", "sh", "exit_code",
    "read_file", "write_file", "file_exists", "getenv", "args", "exit", "panic", "sleep_ms",
    "now_ms", "contains", "has_prefix", "has_suffix", "index_of", "substr", "trim", "upper",
    "lower", "replace", "split", "join", NULL
};

static int is_builtin(const char *n) {
    for (int i = 0; k_builtins[i]; i++)
        if (strcmp(k_builtins[i], n) == 0) return 1;
    return 0;
}

static fsig *declare_func(node *fn, adef *tool_of) {
    fsig *f = xcalloc(1, sizeof(*f));
    f->name = xstrdup(fn->name);
    f->decl = fn;
    f->doc = fn->text;
    f->tool_of = tool_of;
    f->np = (int)fn->list.len;
    f->pt = xcalloc((size_t)f->np + 1, sizeof(*f->pt));
    f->pn = xcalloc((size_t)f->np + 1, sizeof(*f->pn));
    for (int i = 0; i < f->np; i++) {
        node *p = fn->list.items[i];
        f->pn[i] = p->name;
        f->pt[i] = resolve_type(p->type);
        for (int j = 0; j < i; j++)
            if (strcmp(f->pn[j], p->name) == 0) err(p, "duplicate parameter %s", p->name);
    }
    f->ret = resolve_type(fn->type);
    if (tool_of) {
        f->ll = xasprintf("@hl_tool_%s_%s", tool_of->name, f->name);
        f->wrapper = xasprintf("@hl_toolw_%s_%s", tool_of->name, f->name);
        f->params_sd = new_struct(xasprintf("%s.%s.params", tool_of->name, f->name), fn);
        for (int i = 0; i < f->np; i++) add_field(f->params_sd, f->pn[i], f->pt[i], NULL);
        layout(f->params_sd);
    } else {
        if (is_builtin(f->name)) err(fn, "func %s: the name is a built-in function", f->name);
        if (find_func(f->name)) err(fn, "func %s is declared twice", f->name);
        f->ll = xasprintf("@hl_fn_%s", f->name);
    }
    g_funcs = realloc(g_funcs, sizeof(*g_funcs) * (size_t)(g_nfuncs + 1));
    g_funcs[g_nfuncs++] = f;
    return f;
}

/* ---- declarative tools ------------------------------------------------ */

static fsig **g_bound;          /* top-level `tool` declarations */
static int g_nbound;

static fsig *find_bound_tool(const char *name) {
    for (int i = 0; i < g_nbound; i++)
        if (strcmp(g_bound[i]->name, name) == 0) return g_bound[i];
    return NULL;
}

/* Every {{name}} in a template must be a parameter. */
static void check_template(node *at, fsig *f, const char *t) {
    for (const char *p = t ? strstr(t, "{{") : NULL; p; p = strstr(p, "{{")) {
        const char *e = strstr(p + 2, "}}");
        if (!e) break;
        char *name = strndup(p + 2, (size_t)(e - p - 2));
        char *n = name;
        while (*n == ' ') n++;
        for (char *z = n + strlen(n); z > n && z[-1] == ' ';) *--z = '\0';
        int ok = 0;
        for (int i = 0; i < f->np; i++) ok |= strcmp(f->pn[i], n) == 0;
        if (!ok) err(at, "tool %s: {{%s}} is not one of its parameters", f->name, n);
        free(name);
        p = e + 2;
    }
}

static fsig *declare_bound_tool(node *tn) {
    static const char *const kinds[] = {"sh", "http", "print", "read", "write", "agent", NULL};
    const char *kind = tn->a->name;
    int known = 0;
    for (int i = 0; kinds[i]; i++) known |= strcmp(kinds[i], kind) == 0;
    if (!known) {
        err(tn, "tool %s: unknown action '%s' (sh, http, print, read, write, agent)", tn->name, kind);
        return NULL;
    }
    fsig *f = xcalloc(1, sizeof(*f));
    f->name = xstrdup(tn->name);
    f->decl = tn;
    f->doc = tn->text;
    f->binding = xstrdup(kind);
    f->ret = &ty_string;
    if (strcmp(kind, "agent") == 0) {
        if (!tn->d) { err(tn, "tool %s = agent <Name>", tn->name); return NULL; }
        adef *sub = find_agent(tn->d->name);
        if (!sub || sub->is_skill) { err(tn, "tool %s: %s is not an agent", tn->name, tn->d->name); return NULL; }
        if (sub->is_abstract) { err(tn, "tool %s: agent %s is abstract", tn->name, sub->name); return NULL; }
        resolve_agent(sub);
        f->sub = sub;
        f->params_sd = sub->input;      /* the tool's parameters are the agent's input */
        f->np = 0;
        if (!f->doc) f->doc = sub->doc;
        return f;
    }
    if (tn->d || !tn->b) { err(tn, "tool %s = %s \"template\"", tn->name, kind); return NULL; }
    if (strcmp(kind, "write") == 0 && !tn->c) err(tn, "tool %s = write \"path\" \"content\"", tn->name);
    if (strcmp(kind, "write") != 0 && tn->c) err(tn, "tool %s: %s takes one template", tn->name, kind);
    f->templ = xstrdup(tn->b->text);
    f->templ2 = tn->c ? xstrdup(tn->c->text) : NULL;
    f->np = (int)tn->list.len;
    f->pt = xcalloc((size_t)f->np + 1, sizeof(*f->pt));
    f->pn = xcalloc((size_t)f->np + 1, sizeof(*f->pn));
    f->params_sd = new_struct(xasprintf("tool.%s.%d.params", f->name, g_nstructs), tn);
    for (int i = 0; i < f->np; i++) {
        node *p = tn->list.items[i];
        f->pn[i] = p->name;
        f->pt[i] = resolve_type(p->type);
        if (f->pt[i]->k == T_STRUCT || f->pt[i]->k == T_ARRAY)
            err(p, "tool %s: template parameters are int, float, bool or string", f->name);
        add_field(f->params_sd, p->name, f->pt[i], NULL);
    }
    layout(f->params_sd);
    check_template(tn, f, f->templ);
    check_template(tn, f, f->templ2);
    return f;
}

static void declare_all(nodelist *top) {
    /* names first, so declarations can refer to each other in any order */
    for (size_t i = 0; i < top->len; i++) {
        node *n = top->items[i];
        if (n->kind == N_STRUCT) {
            if (find_struct(n->name) || find_agent(n->name)) err(n, "%s is declared twice", n->name);
            new_struct(n->name, n);
        } else if (n->kind == N_AGENT) {
            if (find_struct(n->name) || find_agent(n->name)) err(n, "%s is declared twice", n->name);
            adef *a = xcalloc(1, sizeof(*a));
            a->name = n->name;
            a->doc = n->text;
            a->is_abstract = n->flag != 0;
            a->is_skill = n->flag == 2;
            a->decl = n;
            a->sym = xasprintf("@__agent.%s", n->name);
            g_agents = realloc(g_agents, sizeof(*g_agents) * (size_t)(g_nagents + 1));
            g_agents[g_nagents++] = a;
        }
    }
    for (int i = 0; i < g_nstructs; i++) {
        sdef *sd = g_structs[i];
        node *n = sd->decl;
        for (size_t j = 0; j < n->list.len; j++) {
            node *f = n->list.items[j];
            if (struct_field(sd, f->name) >= 0) err(f, "duplicate field %s", f->name);
            else add_field(sd, f->name, resolve_type(f->type), f->text);
        }
        layout(sd);
    }
    for (size_t i = 0; i < top->len; i++) {
        node *n = top->items[i];
        if (n->kind != N_TOOLBIND) continue;
        if (find_bound_tool(n->name)) { err(n, "tool %s is declared twice", n->name); continue; }
        fsig *f = declare_bound_tool(n);
        if (!f) continue;
        g_bound = realloc(g_bound, sizeof(*g_bound) * (size_t)(g_nbound + 1));
        g_bound[g_nbound++] = f;
    }
    for (int i = 0; i < g_nagents; i++) resolve_agent(g_agents[i]);
    for (size_t i = 0; i < top->len; i++)
        if (top->items[i]->kind == N_FUNC) declare_func(top->items[i], NULL);
}

/* ================================================================== */
/* module output                                                      */
/* ================================================================== */

static strbuf m_types, m_globals, m_funcs;
static char **g_strs;
static int g_nstrs;
static int g_desc_counter;

/* Every string of the program is stored encrypted (ChaCha20, a random key per
 * build) in a writable global, listed in @hl_strtab; the runtime decrypts them
 * in place before main. Prompts, skills and templates are then not readable
 * in the binary file. */
static uint8_t g_key[32], g_mask[32];
static strbuf m_strtab;

static void init_key(void) {
    FILE *r = fopen("/dev/urandom", "rb");
    if (!r || fread(g_key, 1, 32, r) != 32 || fread(g_mask, 1, 32, r) != 32) {
        fprintf(stderr, "holacc: cannot read /dev/urandom\n");
        exit(1);
    }
    fclose(r);
}

static void put_bytes(strbuf *b, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) sb_printf(b, "\\%02X", d[i]);
}

static char *strconst(const char *s) {
    for (int i = 0; i < g_nstrs; i++)
        if (strcmp(g_strs[i], s) == 0) return xasprintf("@.str.%d", i);
    g_strs = realloc(g_strs, sizeof(*g_strs) * (size_t)(g_nstrs + 1));
    g_strs[g_nstrs] = xstrdup(s);
    size_t n = strlen(s) + 1;
    uint8_t *enc = xcalloc(n, 1);
    memcpy(enc, s, n);
    uint8_t nonce[12];
    hl_cc_nonce((uint64_t)g_nstrs, nonce);
    hl_chacha20(g_key, nonce, enc, n);
    sb_printf(&m_globals, "@.str.%d = internal global [%zu x i8] c\"", g_nstrs, n);
    put_bytes(&m_globals, enc, n);
    sb_puts(&m_globals, "\"\n");
    free(enc);
    sb_printf(&m_strtab, "%s{ ptr, i64 } { ptr @.str.%d, i64 %zu }", g_nstrs ? ", " : "", g_nstrs, n);
    return xasprintf("@.str.%d", g_nstrs++);
}

static void emit_strtab(void) {
    uint8_t masked[32];
    for (int i = 0; i < 32; i++) masked[i] = g_key[i] ^ g_mask[i];
    sb_printf(&m_globals, "@hl_strtab = constant [%d x { ptr, i64 }] [%s]\n", g_nstrs,
              m_strtab.data ? m_strtab.data : "");
    if (!g_nstrs) {
        /* an empty table needs zeroinitializer */
        char *p = strstr(m_globals.data, "@hl_strtab = constant [0 x { ptr, i64 }] []");
        if (p) { m_globals.len = (size_t)(p - m_globals.data); m_globals.data[m_globals.len] = '\0';
                 sb_puts(&m_globals, "@hl_strtab = constant [0 x { ptr, i64 }] zeroinitializer\n"); }
    }
    sb_printf(&m_globals, "@hl_nstrs = constant i64 %d\n@hl_strkey = constant [32 x i8] c\"", g_nstrs);
    put_bytes(&m_globals, masked, 32);
    sb_puts(&m_globals, "\"\n@hl_strmask = constant [32 x i8] c\"");
    put_bytes(&m_globals, g_mask, 32);
    sb_puts(&m_globals, "\"\n");
}

static char *opt_strconst(const char *s) { return s ? strconst(s) : xstrdup("null"); }

static char *desc_of(type *t);

static char *struct_desc(sdef *sd) {
    if (sd->desc_sym) return sd->desc_sym;
    sd->desc_sym = xasprintf("@__ty.%s", sd->name);
    strbuf fields = {0};
    for (int i = 0; i < sd->nf; i++) {
        char *fd = desc_of(sd->f[i].t);
        char *nm = strconst(sd->f[i].name);
        char *ds = opt_strconst(sd->f[i].desc);
        sb_printf(&fields, "%s%%hl.field { ptr %s, ptr %s, i64 %lld, ptr %s }", i ? ", " : "",
                  nm, ds, (long long)sd->f[i].off, fd);
    }
    char *nm = strconst(sd->name);
    if (sd->nf) {
        sb_printf(&m_globals, "@__ty.%s.fields = private constant [%d x %%hl.field] [%s]\n", sd->name, sd->nf,
                  fields.data);
        sb_printf(&m_globals, "%s = constant %%hl.type { i64 6, i64 %lld, ptr %s, ptr null, i64 %d, ptr @__ty.%s.fields }\n",
                  sd->desc_sym, (long long)sd->size, nm, sd->nf, sd->name);
    } else {
        sb_printf(&m_globals, "%s = constant %%hl.type { i64 6, i64 %lld, ptr %s, ptr null, i64 0, ptr null }\n",
                  sd->desc_sym, (long long)sd->size, nm);
    }
    sb_free(&fields);
    return sd->desc_sym;
}

static char *desc_of(type *t) {
    static type **arr_t;
    static char **arr_sym;
    static int narr;
    switch (t->k) {
    case T_INT: return "@hl_ty_int";
    case T_FLOAT: return "@hl_ty_float";
    case T_BOOL: return "@hl_ty_bool";
    case T_STRING: return "@hl_ty_string";
    case T_STRUCT: return struct_desc(t->sd);
    case T_ARRAY: {
        for (int i = 0; i < narr; i++)
            if (arr_t[i] == t) return arr_sym[i];
        char *sym = xasprintf("@__ty.arr.%d", g_desc_counter++);
        arr_t = realloc(arr_t, sizeof(*arr_t) * (size_t)(narr + 1));
        arr_sym = realloc(arr_sym, sizeof(*arr_sym) * (size_t)(narr + 1));
        arr_t[narr] = t;
        arr_sym[narr++] = sym;
        char *ed = desc_of(t->elem);
        char *tn = type_name(t);
        sb_printf(&m_globals, "%s = constant %%hl.type { i64 5, i64 8, ptr %s, ptr %s, i64 0, ptr null }\n",
                  sym, strconst(tn), ed);
        free(tn);
        return sym;
    }
    default: return "null";
    }
}

static char *float_lit(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    return xasprintf("0x%016llX", (unsigned long long)bits);
}

/* ================================================================== */
/* function context                                                   */
/* ================================================================== */

typedef struct {
    char *name;
    type *t;
    char *slot;
    int depth;
} local;

typedef struct {
    strbuf allocas, body;
    int tmp, lbl;
    local *locals;
    int nlocals, caplocals, depth;
    type *ret;
    int terminated;
    char *cur;                  /* current block label */
    char *brk[128], *cont[128];
    int nloop;
    char *file;                 /* @.str of the source file name */
} fnctx;

typedef struct {
    type *t;
    char *v;
    int is_const;
} val;

static val mkval(type *t, char *v) { val r = {t, v, 0}; return r; }

static char *tmp(fnctx *f) { return xasprintf("%%t%d", ++f->tmp); }
static char *newlabel(fnctx *f, const char *p) { return xasprintf("%s%d", p, ++f->lbl); }

static void put_label(fnctx *f, const char *l) {
    sb_printf(&f->body, "%s:\n", l);
    f->cur = xstrdup(l);
    f->terminated = 0;
}

static void emit(fnctx *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(fnctx *f, const char *fmt, ...) {
    if (f->terminated) put_label(f, newlabel(f, "dead"));
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) abort();
    va_end(ap);
    sb_printf(&f->body, "  %s\n", s);
    free(s);
}

static void label(fnctx *f, const char *l) {
    if (!f->terminated) sb_printf(&f->body, "  br label %%%s\n", l);
    put_label(f, l);
}

static void br(fnctx *f, const char *l) {
    emit(f, "br label %%%s", l);
    f->terminated = 1;
}

static void condbr(fnctx *f, const char *c, const char *t, const char *e) {
    emit(f, "br i1 %s, label %%%s, label %%%s", c, t, e);
    f->terminated = 1;
}

static char *new_slot(fnctx *f, type *t, const char *hint) {
    char *s = xasprintf("%%l%d.%s", ++f->tmp, hint ? hint : "tmp");
    sb_printf(&f->allocas, "  %s = alloca %s\n", s, lt(t));
    return s;
}

static local *find_local(fnctx *f, const char *name) {
    for (int i = f->nlocals - 1; i >= 0; i--)
        if (strcmp(f->locals[i].name, name) == 0) return &f->locals[i];
    return NULL;
}

static char *declare_local(fnctx *f, node *at, const char *name, type *t) {
    for (int i = f->nlocals - 1; i >= 0 && f->locals[i].depth == f->depth; i--)
        if (strcmp(f->locals[i].name, name) == 0) err(at, "%s is already declared in this block", name);
    if (f->nlocals == f->caplocals) {
        f->caplocals = f->caplocals ? f->caplocals * 2 : 16;
        f->locals = realloc(f->locals, sizeof(*f->locals) * (size_t)f->caplocals);
    }
    char *slot = new_slot(f, t, name);
    local *l = &f->locals[f->nlocals++];
    l->name = xstrdup(name);
    l->t = t;
    l->slot = slot;
    l->depth = f->depth;
    return slot;
}

static void push_scope(fnctx *f) { f->depth++; }
static void pop_scope(fnctx *f) {
    while (f->nlocals && f->locals[f->nlocals - 1].depth == f->depth) f->nlocals--;
    f->depth--;
}

/* ================================================================== */
/* expressions                                                        */
/* ================================================================== */

static val gen_expr(fnctx *f, node *n);
static void gen_stmt(fnctx *f, node *n);
static void gen_block(fnctx *f, node *n);

static val bad(void) { return mkval(&ty_int, xstrdup("0")); }

static val zero_value(fnctx *f, type *t) {
    switch (t->k) {
    case T_INT: return mkval(t, xstrdup("0"));
    case T_FLOAT: return mkval(t, xstrdup("0.0"));
    case T_BOOL: return mkval(t, xstrdup("false"));
    case T_STRING: return mkval(t, strconst(""));
    case T_ARRAY: {
        char *r = tmp(f);
        emit(f, "%s = call ptr @hl_arr_new(i64 %lld)", r, (long long)slot_size(t->elem));
        return mkval(t, r);
    }
    case T_STRUCT: {
        char *r = tmp(f);
        emit(f, "%s = call ptr @hl_alloc(i64 %lld)", r, (long long)t->sd->size);
        return mkval(t, r);
    }
    default: return mkval(t, xstrdup("null"));
    }
}

/* Converts `v` to `want`: exact types pass, an int constant becomes a float. */
static val coerce(fnctx *f, val v, type *want, node *at, const char *what) {
    (void)f;
    if (v.t == want) return v;
    if (want->k == T_FLOAT && v.t->k == T_INT && v.is_const) {
        val r = mkval(&ty_float, float_lit((double)strtoll(v.v, NULL, 10)));
        r.is_const = 1;
        return r;
    }
    char *a = type_name(v.t), *b = type_name(want);
    err(at, "%s: cannot use %s as %s", what, a, b);
    free(a); free(b);
    return zero_value(f, want);
}

static val gen_expr_want(fnctx *f, node *n, type *want, const char *what) {
    return coerce(f, gen_expr(f, n), want, n, what);
}

static char *line_args(fnctx *f, node *n) {
    return xasprintf("ptr %s, i64 %d", f->file, n->line);
}

static char *to_i64(fnctx *f, val b) {
    char *r = tmp(f);
    emit(f, "%s = zext i1 %s to i64", r, b.v);
    return r;
}

static val from_i64_bool(fnctx *f, const char *v) {
    char *r = tmp(f);
    emit(f, "%s = icmp ne i64 %s, 0", r, v);
    return mkval(&ty_bool, r);
}

/* Stores a value into a fresh slot and returns the slot (for runtime calls that take any type). */
static char *spill(fnctx *f, val v) {
    char *s = new_slot(f, v.t, "spill");
    emit(f, "store %s %s, ptr %s", lt(v.t), v.v, s);
    return s;
}

static val to_string(fnctx *f, val v, node *at) {
    char *r = tmp(f);
    switch (v.t->k) {
    case T_STRING: return v;
    case T_INT: emit(f, "%s = call ptr @hl_int_to_str(i64 %s)", r, v.v); break;
    case T_FLOAT: emit(f, "%s = call ptr @hl_float_to_str(double %s)", r, v.v); break;
    case T_BOOL: emit(f, "%s = call ptr @hl_bool_to_str(i64 %s)", r, to_i64(f, v)); break;
    case T_ARRAY: case T_STRUCT:
        emit(f, "%s = call ptr @hl_to_json(ptr %s, ptr %s)", r, desc_of(v.t), spill(f, v));
        break;
    default: err(at, "cannot convert this value to a string"); return mkval(&ty_string, strconst(""));
    }
    return mkval(&ty_string, r);
}

static void print_value(fnctx *f, val v, node *at) {
    switch (v.t->k) {
    case T_STRING: emit(f, "call void @hl_print_str(ptr %s)", v.v); break;
    case T_INT: emit(f, "call void @hl_print_int(i64 %s)", v.v); break;
    case T_FLOAT: emit(f, "call void @hl_print_float(double %s)", v.v); break;
    case T_BOOL: emit(f, "call void @hl_print_bool(i64 %s)", to_i64(f, v)); break;
    case T_ARRAY: case T_STRUCT:
        emit(f, "call void @hl_print_value(ptr %s, ptr %s)", desc_of(v.t), spill(f, v));
        break;
    default: err(at, "cannot print this value");
    }
}

static char *struct_ll(sdef *sd) { return xasprintf("%%\"%s\"", sd->name); }

static char *field_addr(fnctx *f, val base, int k, node *at) {
    emit(f, "call void @hl_check_nil(ptr %s, %s)", base.v, line_args(f, at));
    char *r = tmp(f);
    emit(f, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", r, struct_ll(base.t->sd), base.v, k);
    return r;
}

static char *elem_addr(fnctx *f, val arr, char *idx, node *at) {
    char *r = tmp(f);
    emit(f, "%s = call ptr @hl_arr_at(ptr %s, i64 %s, %s)", r, arr.v, idx, line_args(f, at));
    return r;
}

static val load(fnctx *f, type *t, const char *addr) {
    char *r = tmp(f);
    emit(f, "%s = load %s, ptr %s", r, lt(t), addr);
    return mkval(t, r);
}

/* The address of an assignable expression. */
static char *gen_addr(fnctx *f, node *n, type **out) {
    *out = &ty_int;
    if (n->kind == N_IDENT) {
        local *l = find_local(f, n->name);
        if (!l) { err(n, "undefined: %s", n->name); return new_slot(f, &ty_int, "bad"); }
        *out = l->t;
        return l->slot;
    }
    if (n->kind == N_FIELD) {
        val b = gen_expr(f, n->a);
        if (b.t->k != T_STRUCT) { err(n, "%s has no fields", type_name(b.t)); return new_slot(f, &ty_int, "bad"); }
        int k = struct_field(b.t->sd, n->name);
        if (k < 0) { err(n, "%s has no field %s", b.t->sd->name, n->name); return new_slot(f, &ty_int, "bad"); }
        *out = b.t->sd->f[k].t;
        return field_addr(f, b, k, n);
    }
    if (n->kind == N_INDEX) {
        val b = gen_expr(f, n->a);
        if (b.t->k != T_ARRAY) {
            err(n, b.t->k == T_STRING ? "strings cannot be changed in place" : "only arrays can be indexed");
            return new_slot(f, &ty_int, "bad");
        }
        val i = gen_expr_want(f, n->b, &ty_int, "index");
        *out = b.t->elem;
        return elem_addr(f, b, i.v, n);
    }
    err(n, "cannot assign to this expression");
    return new_slot(f, &ty_int, "bad");
}

static val gen_binary(fnctx *f, node *n) {
    int op = (int)n->ival;
    if (op == OP_AND || op == OP_OR) {
        val a = gen_expr_want(f, n->a, &ty_bool, op == OP_AND ? "&&" : "||");
        char *from = f->cur;
        char *rhs = newlabel(f, "sc.rhs"), *end = newlabel(f, "sc.end");
        if (op == OP_AND) condbr(f, a.v, rhs, end);
        else condbr(f, a.v, end, rhs);
        put_label(f, rhs);
        val b = gen_expr_want(f, n->b, &ty_bool, op == OP_AND ? "&&" : "||");
        char *rhs_end = f->cur;
        br(f, end);
        put_label(f, end);
        char *r = tmp(f);
        emit(f, "%s = phi i1 [ %s, %%%s ], [ %s, %%%s ]", r, op == OP_AND ? "false" : "true", from, b.v, rhs_end);
        return mkval(&ty_bool, r);
    }

    val a = gen_expr(f, n->a);
    val b = gen_expr(f, n->b);
    if (a.t != b.t) {
        if (a.t->k == T_FLOAT && b.t->k == T_INT && b.is_const) b = coerce(f, b, &ty_float, n, "operand");
        else if (b.t->k == T_FLOAT && a.t->k == T_INT && a.is_const) a = coerce(f, a, &ty_float, n, "operand");
    }
    if (a.t != b.t) {
        char *x = type_name(a.t), *y = type_name(b.t);
        err(n, "mismatched types %s and %s", x, y);
        free(x); free(y);
        return bad();
    }
    type *t = a.t;
    char *r = tmp(f);
    const char *cmp = NULL;
    switch (op) {
    case OP_EQ: cmp = "eq"; break;
    case OP_NE: cmp = "ne"; break;
    case '<': cmp = "lt"; break;
    case OP_LE: cmp = "le"; break;
    case '>': cmp = "gt"; break;
    case OP_GE: cmp = "ge"; break;
    }

    if (t->k == T_INT) {
        if (cmp) {
            const char *p = !strcmp(cmp, "eq") ? "eq" : !strcmp(cmp, "ne") ? "ne" :
                            !strcmp(cmp, "lt") ? "slt" : !strcmp(cmp, "le") ? "sle" :
                            !strcmp(cmp, "gt") ? "sgt" : "sge";
            emit(f, "%s = icmp %s i64 %s, %s", r, p, a.v, b.v);
            return mkval(&ty_bool, r);
        }
        switch (op) {
        case '+': emit(f, "%s = add i64 %s, %s", r, a.v, b.v); break;
        case '-': emit(f, "%s = sub i64 %s, %s", r, a.v, b.v); break;
        case '*': emit(f, "%s = mul i64 %s, %s", r, a.v, b.v); break;
        case '/': emit(f, "%s = call i64 @hl_div(i64 %s, i64 %s, %s)", r, a.v, b.v, line_args(f, n)); break;
        case '%': emit(f, "%s = call i64 @hl_mod(i64 %s, i64 %s, %s)", r, a.v, b.v, line_args(f, n)); break;
        default: err(n, "bad operator for int"); return bad();
        }
        return mkval(t, r);
    }
    if (t->k == T_FLOAT) {
        if (cmp) {
            const char *p = !strcmp(cmp, "eq") ? "oeq" : !strcmp(cmp, "ne") ? "une" :
                            !strcmp(cmp, "lt") ? "olt" : !strcmp(cmp, "le") ? "ole" :
                            !strcmp(cmp, "gt") ? "ogt" : "oge";
            emit(f, "%s = fcmp %s double %s, %s", r, p, a.v, b.v);
            return mkval(&ty_bool, r);
        }
        switch (op) {
        case '+': emit(f, "%s = fadd double %s, %s", r, a.v, b.v); break;
        case '-': emit(f, "%s = fsub double %s, %s", r, a.v, b.v); break;
        case '*': emit(f, "%s = fmul double %s, %s", r, a.v, b.v); break;
        case '/': emit(f, "%s = fdiv double %s, %s", r, a.v, b.v); break;
        case '%': emit(f, "%s = frem double %s, %s", r, a.v, b.v); break;
        default: err(n, "bad operator for float"); return bad();
        }
        return mkval(t, r);
    }
    if (t->k == T_STRING) {
        if (op == '+') {
            emit(f, "%s = call ptr @hl_str_concat(ptr %s, ptr %s)", r, a.v, b.v);
            return mkval(t, r);
        }
        if (cmp) {
            emit(f, "%s = call i64 @hl_str_cmp(ptr %s, ptr %s)", r, a.v, b.v);
            char *r2 = tmp(f);
            const char *p = !strcmp(cmp, "eq") ? "eq" : !strcmp(cmp, "ne") ? "ne" :
                            !strcmp(cmp, "lt") ? "slt" : !strcmp(cmp, "le") ? "sle" :
                            !strcmp(cmp, "gt") ? "sgt" : "sge";
            emit(f, "%s = icmp %s i64 %s, 0", r2, p, r);
            return mkval(&ty_bool, r2);
        }
        err(n, "bad operator for string");
        return bad();
    }
    if (t->k == T_BOOL && (op == OP_EQ || op == OP_NE)) {
        emit(f, "%s = icmp %s i1 %s, %s", r, op == OP_EQ ? "eq" : "ne", a.v, b.v);
        return mkval(&ty_bool, r);
    }
    char *x = type_name(t);
    err(n, "operator not defined on %s", x);
    free(x);
    return bad();
}

static void want_args(node *n, size_t k, const char *name) {
    if (n->list.len != k) err(n, "%s takes %zu argument%s, got %zu", name, k, k == 1 ? "" : "s", n->list.len);
}

/* A call to a runtime function with string/int arguments: sig like "s:ss" (ret:args). */
static val call_rt(fnctx *f, node *n, const char *fn, const char *sig) {
    const char *colon = strchr(sig, ':');
    size_t nargs = strlen(colon + 1);
    want_args(n, nargs, n->a->name);
    strbuf args = {0};
    for (size_t i = 0; i < nargs && i < n->list.len; i++) {
        char c = colon[1 + i];
        type *t = c == 's' ? &ty_string : c == 'i' ? &ty_int : array_of(&ty_string);
        val v = gen_expr_want(f, n->list.items[i], t, n->a->name);
        sb_printf(&args, "%s%s %s", i ? ", " : "", c == 'i' ? "i64" : "ptr", v.v);
    }
    if (n->list.len != nargs) {
        sb_free(&args);
        return bad();
    }
    char rc = sig[0];
    if (rc == 'v') {
        emit(f, "call void @%s(%s)", fn, args.data ? args.data : "");
        sb_free(&args);
        return mkval(&ty_void, xstrdup(""));
    }
    char *r = tmp(f);
    const char *rt = rc == 'i' || rc == 'b' ? "i64" : "ptr";
    emit(f, "%s = call %s @%s(%s)", r, rt, fn, args.data ? args.data : "");
    sb_free(&args);
    if (rc == 'b') return from_i64_bool(f, r);
    if (rc == 'i') return mkval(&ty_int, r);
    if (rc == 'a') return mkval(array_of(&ty_string), r);
    return mkval(&ty_string, r);
}

static val gen_builtin(fnctx *f, node *n) {
    const char *b = n->a->name;
    if (strcmp(b, "print") == 0 || strcmp(b, "println") == 0) {
        for (size_t i = 0; i < n->list.len; i++) {
            if (i && b[5] == 'l') emit(f, "call void @hl_print_space()");
            print_value(f, gen_expr(f, n->list.items[i]), n->list.items[i]);
        }
        if (b[5] == 'l') emit(f, "call void @hl_print_nl()");
        return mkval(&ty_void, xstrdup(""));
    }
    if (strcmp(b, "len") == 0) {
        want_args(n, 1, b);
        if (n->list.len != 1) return bad();
        val v = gen_expr(f, n->list.items[0]);
        char *r = tmp(f);
        if (v.t->k == T_STRING) emit(f, "%s = call i64 @hl_str_len(ptr %s)", r, v.v);
        else if (v.t->k == T_ARRAY) emit(f, "%s = call i64 @hl_arr_len(ptr %s)", r, v.v);
        else { err(n, "len needs a string or an array"); return bad(); }
        return mkval(&ty_int, r);
    }
    if (strcmp(b, "append") == 0) {
        if (n->list.len < 2) { err(n, "append needs an array and at least one value"); return bad(); }
        val a = gen_expr(f, n->list.items[0]);
        if (a.t->k != T_ARRAY) { err(n, "append needs an array first"); return bad(); }
        char *cur = a.v;
        for (size_t i = 1; i < n->list.len; i++) {
            val v = gen_expr_want(f, n->list.items[i], a.t->elem, "append");
            char *g = tmp(f);
            emit(f, "%s = call ptr @hl_arr_grow(ptr %s, i64 %lld)", g, cur, (long long)slot_size(a.t->elem));
            char *len = tmp(f), *idx = tmp(f);
            emit(f, "%s = call i64 @hl_arr_len(ptr %s)", len, g);
            emit(f, "%s = sub i64 %s, 1", idx, len);
            char *slot = elem_addr(f, mkval(a.t, g), idx, n);
            emit(f, "store %s %s, ptr %s", lt(a.t->elem), v.v, slot);
            cur = g;
        }
        return mkval(a.t, cur);
    }
    if (strcmp(b, "str") == 0) {
        want_args(n, 1, b);
        if (n->list.len != 1) return bad();
        return to_string(f, gen_expr(f, n->list.items[0]), n);
    }
    if (strcmp(b, "json") == 0) {
        want_args(n, 1, b);
        if (n->list.len != 1) return bad();
        val v = gen_expr(f, n->list.items[0]);
        if (v.t->k == T_VOID || v.t->k == T_AGENT) { err(n, "json needs a value"); return bad(); }
        char *r = tmp(f);
        emit(f, "%s = call ptr @hl_to_json(ptr %s, ptr %s)", r, desc_of(v.t), spill(f, v));
        return mkval(&ty_string, r);
    }
    if (strcmp(b, "int") == 0 || strcmp(b, "float") == 0) {
        want_args(n, 1, b);
        if (n->list.len != 1) return bad();
        val v = gen_expr(f, n->list.items[0]);
        int to_int = b[0] == 'i';
        char *r = tmp(f);
        if (to_int) {
            if (v.t->k == T_INT) return v;
            if (v.t->k == T_FLOAT) emit(f, "%s = fptosi double %s to i64", r, v.v);
            else if (v.t->k == T_STRING) emit(f, "%s = call i64 @hl_str_to_int(ptr %s)", r, v.v);
            else if (v.t->k == T_BOOL) emit(f, "%s = zext i1 %s to i64", r, v.v);
            else { err(n, "cannot convert to int"); return bad(); }
            return mkval(&ty_int, r);
        }
        if (v.t->k == T_FLOAT) return v;
        if (v.t->k == T_INT) emit(f, "%s = sitofp i64 %s to double", r, v.v);
        else if (v.t->k == T_STRING) emit(f, "%s = call double @hl_str_to_float(ptr %s)", r, v.v);
        else { err(n, "cannot convert to float"); return mkval(&ty_float, xstrdup("0.0")); }
        return mkval(&ty_float, r);
    }
    if (strcmp(b, "exit") == 0) {
        val v = (want_args(n, 1, b), n->list.len == 1 ? gen_expr_want(f, n->list.items[0], &ty_int, b) : bad());
        emit(f, "call void @hl_exit(i64 %s)", v.v);
        emit(f, "unreachable");
        f->terminated = 1;
        return mkval(&ty_void, xstrdup(""));
    }
    if (strcmp(b, "panic") == 0) {
        val v = (want_args(n, 1, b), n->list.len == 1 ? gen_expr(f, n->list.items[0]) : bad());
        v = to_string(f, v, n);
        emit(f, "call void @hl_panic_at(ptr %s, %s)", v.v, line_args(f, n));
        emit(f, "unreachable");
        f->terminated = 1;
        return mkval(&ty_void, xstrdup(""));
    }
    if (strcmp(b, "sh") == 0) return call_rt(f, n, "hl_sh", "s:s");
    if (strcmp(b, "exit_code") == 0) return call_rt(f, n, "hl_exit_code", "i:");
    if (strcmp(b, "read_file") == 0) return call_rt(f, n, "hl_read_file", "s:s");
    if (strcmp(b, "write_file") == 0) return call_rt(f, n, "hl_write_file", "b:ss");
    if (strcmp(b, "file_exists") == 0) return call_rt(f, n, "hl_file_exists", "b:s");
    if (strcmp(b, "getenv") == 0) return call_rt(f, n, "hl_getenv", "s:s");
    if (strcmp(b, "args") == 0) return call_rt(f, n, "hl_args", "a:");
    if (strcmp(b, "sleep_ms") == 0) return call_rt(f, n, "hl_sleep_ms", "v:i");
    if (strcmp(b, "now_ms") == 0) return call_rt(f, n, "hl_now_ms", "i:");
    if (strcmp(b, "contains") == 0) return call_rt(f, n, "hl_contains", "b:ss");
    if (strcmp(b, "has_prefix") == 0) return call_rt(f, n, "hl_has_prefix", "b:ss");
    if (strcmp(b, "has_suffix") == 0) return call_rt(f, n, "hl_has_suffix", "b:ss");
    if (strcmp(b, "index_of") == 0) return call_rt(f, n, "hl_index_of", "i:ss");
    if (strcmp(b, "substr") == 0) return call_rt(f, n, "hl_substr", "s:sii");
    if (strcmp(b, "trim") == 0) return call_rt(f, n, "hl_trim", "s:s");
    if (strcmp(b, "upper") == 0) return call_rt(f, n, "hl_upper", "s:s");
    if (strcmp(b, "lower") == 0) return call_rt(f, n, "hl_lower", "s:s");
    if (strcmp(b, "replace") == 0) return call_rt(f, n, "hl_replace", "s:sss");
    if (strcmp(b, "split") == 0) return call_rt(f, n, "hl_split", "a:ss");
    if (strcmp(b, "join") == 0) return call_rt(f, n, "hl_join", "s:as");
    err(n, "unknown built-in %s", b);
    return bad();
}

static val run_agent(fnctx *f, adef *a, char *input, node *at) {
    if (a->is_abstract) err(at, "agent %s is abstract: extend it, it cannot run by itself", a->name);
    char *r = tmp(f);
    emit(f, "%s = call ptr @hl_agent_run(ptr %s, ptr %s)", r, a->sym, input);
    return mkval(a->output ? struct_type(a->output) : &ty_string, r);
}

static val gen_call(fnctx *f, node *n) {
    node *callee = n->a;
    /* methods: Agent.ask(text), input.run() */
    if (callee->kind == N_FIELD) {
        node *base = callee->a;
        if (base->kind == N_IDENT && !find_local(f, base->name) && find_agent(base->name)) {
            adef *a = find_agent(base->name);
            if (a->is_skill) { err(n, "%s is a skill, not an agent; list it in an agent's skills", a->name); return bad(); }
            if (strcmp(callee->name, "ask") != 0) {
                err(n, "agent %s: call %s.ask(text) or %s{...}.run()", a->name, a->name, a->name);
                return bad();
            }
            if (a->input->nf != 1 || a->input->f[0].t != &ty_string) {
                err(n, "%s.ask needs an agent with one string input; use %s{...}.run()", a->name, a->name);
                return bad();
            }
            want_args(n, 1, "ask");
            if (n->list.len != 1) return bad();
            val s = gen_expr_want(f, n->list.items[0], &ty_string, "ask");
            char *p = tmp(f);
            emit(f, "%s = call ptr @hl_alloc(i64 %lld)", p, (long long)a->input->size);
            char *fa = tmp(f);
            emit(f, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 0", fa, struct_ll(a->input), p);
            emit(f, "store ptr %s, ptr %s", s.v, fa);
            return run_agent(f, a, p, n);
        }
        val b = gen_expr(f, base);
        if (b.t->k == T_STRUCT && b.t->sd->input_of && strcmp(callee->name, "run") == 0) {
            if (n->list.len) err(n, "run takes no arguments");
            emit(f, "call void @hl_check_nil(ptr %s, %s)", b.v, line_args(f, n));
            return run_agent(f, b.t->sd->input_of, b.v, n);
        }
        char *tn = type_name(b.t);
        err(n, "%s has no method %s", tn, callee->name);
        free(tn);
        return bad();
    }
    if (callee->kind != N_IDENT) { err(n, "cannot call this expression"); return bad(); }
    fsig *fs = find_func(callee->name);
    if (!fs) {
        if (is_builtin(callee->name)) return gen_builtin(f, n);
        err(n, "undefined function %s", callee->name);
        return bad();
    }
    if ((int)n->list.len != fs->np) {
        err(n, "%s takes %d argument%s, got %zu", fs->name, fs->np, fs->np == 1 ? "" : "s", n->list.len);
        return bad();
    }
    strbuf args = {0};
    for (int i = 0; i < fs->np; i++) {
        val v = gen_expr_want(f, n->list.items[i], fs->pt[i], fs->name);
        sb_printf(&args, "%s%s %s", i ? ", " : "", lt(fs->pt[i]), v.v);
    }
    val res;
    if (fs->ret->k == T_VOID) {
        emit(f, "call void %s(%s)", fs->ll, args.data ? args.data : "");
        res = mkval(&ty_void, xstrdup(""));
    } else {
        char *r = tmp(f);
        emit(f, "%s = call %s %s(%s)", r, lt(fs->ret), fs->ll, args.data ? args.data : "");
        res = mkval(fs->ret, r);
    }
    sb_free(&args);
    return res;
}

static val gen_composite(fnctx *f, node *n) {
    /* Agent{...} builds the agent's input */
    adef *ag = find_agent(n->type->name);
    if (ag && ag->is_skill) { err(n, "%s is a skill, not an agent", ag->name); return bad(); }
    type *t = ag ? struct_type(ag->input) : resolve_type(n->type);
    if (t->k != T_STRUCT) { err(n, "%s is not a struct", n->type->name); return bad(); }
    sdef *sd = t->sd;
    char *p = tmp(f);
    emit(f, "%s = call ptr @hl_alloc(i64 %lld)", p, (long long)sd->size);
    int *seen = xcalloc((size_t)sd->nf + 1, sizeof(int));
    for (size_t i = 0; i < n->list.len; i++) {
        node *kv = n->list.items[i];
        int k = struct_field(sd, kv->name);
        if (k < 0) { err(kv, "%s has no field %s", sd->name, kv->name); continue; }
        if (seen[k]++) err(kv, "field %s set twice", kv->name);
        val v = gen_expr_want(f, kv->a, sd->f[k].t, kv->name);
        char *fa = tmp(f);
        emit(f, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", fa, struct_ll(sd), p, k);
        emit(f, "store %s %s, ptr %s", lt(sd->f[k].t), v.v, fa);
    }
    /* unset string fields are "" (NULL), arrays empty (NULL), structs nil */
    free(seen);
    return mkval(t, p);
}

static val gen_expr(fnctx *f, node *n) {
    switch (n->kind) {
    case N_INT: { val v = mkval(&ty_int, xasprintf("%lld", (long long)n->ival)); v.is_const = 1; return v; }
    case N_FLOAT: { val v = mkval(&ty_float, float_lit(n->fval)); v.is_const = 1; return v; }
    case N_BOOL: return mkval(&ty_bool, xstrdup(n->ival ? "true" : "false"));
    case N_STR: return mkval(&ty_string, strconst(n->text));
    case N_IDENT: {
        local *l = find_local(f, n->name);
        if (l) return load(f, l->t, l->slot);
        if (find_agent(n->name)) return mkval(agent_type(find_agent(n->name)), xstrdup("null"));
        err(n, "undefined: %s", n->name);
        return bad();
    }
    case N_BINARY: return gen_binary(f, n);
    case N_UNARY: {
        val a = gen_expr(f, n->a);
        char *r = tmp(f);
        if (n->ival == '!') {
            if (a.t->k != T_BOOL) { err(n, "! needs a bool"); return bad(); }
            emit(f, "%s = xor i1 %s, true", r, a.v);
            return mkval(&ty_bool, r);
        }
        if (a.t->k == T_INT) {
            if (a.is_const) { val v = mkval(&ty_int, xasprintf("%lld", -strtoll(a.v, NULL, 10))); v.is_const = 1; return v; }
            emit(f, "%s = sub i64 0, %s", r, a.v);
        } else if (a.t->k == T_FLOAT) {
            emit(f, "%s = fneg double %s", r, a.v);
        } else { err(n, "- needs a number"); return bad(); }
        return mkval(a.t, r);
    }
    case N_CALL: return gen_call(f, n);
    case N_FIELD: {
        val b = gen_expr(f, n->a);
        if (b.t->k == T_AGENT) { err(n, "agent %s: use %s.ask(text) or %s{...}.run()", b.t->ad->name, b.t->ad->name, b.t->ad->name); return bad(); }
        if (b.t->k != T_STRUCT) { char *tn = type_name(b.t); err(n, "%s has no field %s", tn, n->name); free(tn); return bad(); }
        int k = struct_field(b.t->sd, n->name);
        if (k < 0) { err(n, "%s has no field %s", b.t->sd->name, n->name); return bad(); }
        return load(f, b.t->sd->f[k].t, field_addr(f, b, k, n));
    }
    case N_INDEX: {
        val b = gen_expr(f, n->a);
        val i = gen_expr_want(f, n->b, &ty_int, "index");
        if (b.t->k == T_STRING) {
            char *r = tmp(f);
            emit(f, "%s = call i64 @hl_str_at(ptr %s, i64 %s, %s)", r, b.v, i.v, line_args(f, n));
            return mkval(&ty_int, r);
        }
        if (b.t->k != T_ARRAY) { err(n, "only arrays and strings can be indexed"); return bad(); }
        return load(f, b.t->elem, elem_addr(f, b, i.v, n));
    }
    case N_COMPOSITE: return gen_composite(f, n);
    case N_ARRAYLIT: {
        type *et = resolve_type(n->type);
        type *at = array_of(et);
        char *a = tmp(f);
        emit(f, "%s = call ptr @hl_arr_new(i64 %lld)", a, (long long)slot_size(et));
        for (size_t i = 0; i < n->list.len; i++) {
            val v = gen_expr_want(f, n->list.items[i], et, "array element");
            emit(f, "call ptr @hl_arr_grow(ptr %s, i64 %lld)", a, (long long)slot_size(et));
            char *slot = elem_addr(f, mkval(at, a), xasprintf("%zu", i), n);
            emit(f, "store %s %s, ptr %s", lt(et), v.v, slot);
        }
        return mkval(at, a);
    }
    default:
        err(n, "not an expression");
        return bad();
    }
}

/* ================================================================== */
/* statements                                                         */
/* ================================================================== */

static void store_to(fnctx *f, node *target, val v, node *at) {
    type *t;
    char *addr = gen_addr(f, target, &t);
    v = coerce(f, v, t, at, "assignment");
    emit(f, "store %s %s, ptr %s", lt(t), v.v, addr);
}

static void gen_if(fnctx *f, node *n) {
    val c = gen_expr_want(f, n->a, &ty_bool, "if condition");
    char *then_l = newlabel(f, "if.then"), *else_l = newlabel(f, "if.else"), *end = newlabel(f, "if.end");
    condbr(f, c.v, then_l, n->c ? else_l : end);
    put_label(f, then_l);
    gen_block(f, n->b);
    if (!f->terminated) br(f, end);
    if (n->c) {
        put_label(f, else_l);
        if (n->c->kind == N_IF) gen_if(f, n->c);
        else gen_block(f, n->c);
        if (!f->terminated) br(f, end);
    }
    put_label(f, end);
}

static void gen_for(fnctx *f, node *n) {
    push_scope(f);
    if (n->a) gen_stmt(f, n->a);
    char *cond = newlabel(f, "for.cond"), *body = newlabel(f, "for.body"),
         *post = newlabel(f, "for.post"), *end = newlabel(f, "for.end");
    label(f, cond);
    if (n->b) {
        val c = gen_expr_want(f, n->b, &ty_bool, "for condition");
        condbr(f, c.v, body, end);
    } else {
        br(f, body);
    }
    put_label(f, body);
    f->brk[f->nloop] = end;
    f->cont[f->nloop++] = post;
    gen_block(f, n->d);
    f->nloop--;
    label(f, post);
    if (n->c) gen_stmt(f, n->c);
    br(f, cond);
    put_label(f, end);
    pop_scope(f);
}

static void gen_range(fnctx *f, node *n) {
    val arr = gen_expr(f, n->a);
    if (arr.t->k != T_ARRAY && arr.t->k != T_STRING) { err(n, "range needs an array or a string"); return; }
    int is_str = arr.t->k == T_STRING;
    type *et = is_str ? &ty_int : arr.t->elem;
    push_scope(f);
    char *arrslot = spill(f, arr);
    char *idx = new_slot(f, &ty_int, "range.i");
    emit(f, "store i64 0, ptr %s", idx);
    char *islot = strcmp(n->name, "_") ? declare_local(f, n, n->name, &ty_int) : NULL;
    char *vslot = n->text && strcmp(n->text, "_") ? declare_local(f, n, n->text, et) : NULL;
    char *cond = newlabel(f, "range.cond"), *body = newlabel(f, "range.body"),
         *post = newlabel(f, "range.post"), *end = newlabel(f, "range.end");
    label(f, cond);
    val a = load(f, arr.t, arrslot);
    val i = load(f, &ty_int, idx);
    char *len = tmp(f), *c = tmp(f);
    emit(f, "%s = call i64 @%s(ptr %s)", len, is_str ? "hl_str_len" : "hl_arr_len", a.v);
    emit(f, "%s = icmp slt i64 %s, %s", c, i.v, len);
    condbr(f, c, body, end);
    put_label(f, body);
    if (islot) emit(f, "store i64 %s, ptr %s", i.v, islot);
    if (vslot) {
        val v;
        if (is_str) {
            char *r = tmp(f);
            emit(f, "%s = call i64 @hl_str_at(ptr %s, i64 %s, %s)", r, a.v, i.v, line_args(f, n));
            v = mkval(&ty_int, r);
        } else {
            v = load(f, et, elem_addr(f, a, i.v, n));
        }
        emit(f, "store %s %s, ptr %s", lt(et), v.v, vslot);
    }
    f->brk[f->nloop] = end;
    f->cont[f->nloop++] = post;
    gen_block(f, n->d);
    f->nloop--;
    label(f, post);
    val i2 = load(f, &ty_int, idx);
    char *inc = tmp(f);
    emit(f, "%s = add i64 %s, 1", inc, i2.v);
    emit(f, "store i64 %s, ptr %s", inc, idx);
    br(f, cond);
    put_label(f, end);
    pop_scope(f);
}

static void gen_stmt(fnctx *f, node *n) {
    switch (n->kind) {
    case N_EXPRSTMT: {
        node *e = n->a;
        if (e->kind != N_CALL) err(n, "this expression is not used");
        gen_expr(f, e);
        break;
    }
    case N_DEFINE: {
        val v = gen_expr(f, n->a);
        if (v.t->k == T_VOID) { err(n, "%s := ... needs a value; the call returns nothing", n->name); break; }
        if (v.t->k == T_AGENT) { err(n, "an agent is not a value; use %s.ask(text) or %s{...}.run()", v.t->ad->name, v.t->ad->name); break; }
        if (strcmp(n->name, "_") == 0) break;
        char *slot = declare_local(f, n, n->name, v.t);
        emit(f, "store %s %s, ptr %s", lt(v.t), v.v, slot);
        break;
    }
    case N_VAR: {
        type *t = n->type ? resolve_type(n->type) : NULL;
        val v;
        if (n->a) {
            v = t ? gen_expr_want(f, n->a, t, n->name) : gen_expr(f, n->a);
            if (!t) t = v.t;
        } else {
            v = zero_value(f, t);
        }
        if (t->k == T_VOID || t->k == T_AGENT) { err(n, "var %s needs a value type", n->name); break; }
        char *slot = declare_local(f, n, n->name, t);
        emit(f, "store %s %s, ptr %s", lt(t), v.v, slot);
        break;
    }
    case N_ASSIGN: {
        if (n->ival == '=') {
            if (n->a->kind == N_IDENT && strcmp(n->a->name, "_") == 0) { gen_expr(f, n->b); break; }
            val v = gen_expr(f, n->b);
            store_to(f, n->a, v, n);
            break;
        }
        node bin = *n;
        bin.kind = N_BINARY;
        bin.a = n->a;
        bin.b = n->b;
        val v = gen_binary(f, &bin);
        store_to(f, n->a, v, n);
        break;
    }
    case N_INCDEC: {
        type *t;
        char *addr = gen_addr(f, n->a, &t);
        if (t->k != T_INT) { err(n, "++ and -- need an int"); break; }
        val v = load(f, t, addr);
        char *r = tmp(f);
        emit(f, "%s = add i64 %s, %lld", r, v.v, (long long)n->ival);
        emit(f, "store i64 %s, ptr %s", r, addr);
        break;
    }
    case N_IF: gen_if(f, n); break;
    case N_FOR: gen_for(f, n); break;
    case N_RANGE: gen_range(f, n); break;
    case N_BLOCK:
        push_scope(f);
        gen_block(f, n);
        pop_scope(f);
        break;
    case N_RETURN:
        if (f->ret->k == T_VOID) {
            if (n->a) err(n, "this function returns nothing");
            emit(f, "ret void");
        } else if (!n->a) {
            err(n, "missing return value");
            emit(f, "ret %s %s", lt(f->ret), zero_value(f, f->ret).v);
        } else {
            val v = gen_expr_want(f, n->a, f->ret, "return");
            emit(f, "ret %s %s", lt(f->ret), v.v);
        }
        f->terminated = 1;
        break;
    case N_BREAK:
    case N_CONTINUE:
        if (!f->nloop) { err(n, "%s outside a loop", n->kind == N_BREAK ? "break" : "continue"); break; }
        br(f, n->kind == N_BREAK ? f->brk[f->nloop - 1] : f->cont[f->nloop - 1]);
        break;
    default:
        err(n, "unexpected statement");
    }
}

static void gen_block(fnctx *f, node *n) {
    push_scope(f);
    for (size_t i = 0; i < n->list.len; i++) gen_stmt(f, n->list.items[i]);
    pop_scope(f);
}

static void gen_function(fsig *fs) {
    fnctx f = {0};
    node *d = fs->decl;
    f.ret = fs->ret;
    f.file = strconst(d->file ? d->file : "?");
    strbuf params = {0};
    for (int i = 0; i < fs->np; i++) sb_printf(&params, "%s%s %%a.%s", i ? ", " : "", lt(fs->pt[i]), fs->pn[i]);
    push_scope(&f);
    put_label(&f, "start");
    for (int i = 0; i < fs->np; i++) {
        if (strcmp(fs->pn[i], "_") == 0) continue;
        char *slot = declare_local(&f, d, fs->pn[i], fs->pt[i]);
        emit(&f, "store %s %%a.%s, ptr %s", lt(fs->pt[i]), fs->pn[i], slot);
    }
    gen_block(&f, d->a);
    if (!f.terminated) {
        if (fs->ret->k == T_VOID) emit(&f, "ret void");
        else {
            emit(&f, "call void @hl_panic_at(ptr %s, %s)", strconst("missing return at the end of the function"),
                 line_args(&f, d));
            emit(&f, "unreachable");
        }
        f.terminated = 1;
    }
    sb_printf(&m_funcs, "\ndefine %s%s %s(%s) {\nentry:\n%s  br label %%start\n%s}\n",
              fs->tool_of ? "internal " : "", lt(fs->ret), fs->ll, params.data ? params.data : "",
              f.allocas.data ? f.allocas.data : "", f.body.data);
    sb_free(&params);
    sb_free(&f.allocas);
    sb_free(&f.body);
}

/* A tool's wrapper: unpacks the parameter struct the runtime decoded from
 * the model's JSON arguments, calls the tool and returns its result as text. */
static void gen_tool_wrapper(fsig *fs) {
    strbuf b = {0};
    strbuf args = {0};
    char *sl = struct_ll(fs->params_sd);
    sb_printf(&b, "\ndefine internal ptr %s(ptr %%p) {\nentry:\n", fs->wrapper);
    for (int i = 0; i < fs->np; i++) {
        sb_printf(&b, "  %%f%d = getelementptr inbounds %s, ptr %%p, i32 0, i32 %d\n", i, sl, i);
        sb_printf(&b, "  %%v%d = load %s, ptr %%f%d\n", i, lt(fs->pt[i]), i);
        sb_printf(&args, "%s%s %%v%d", i ? ", " : "", lt(fs->pt[i]), i);
    }
    if (fs->ret->k == T_VOID) {
        sb_printf(&b, "  call void %s(%s)\n  ret ptr %s\n}\n", fs->ll, args.data ? args.data : "", strconst("done"));
    } else {
        sb_printf(&b, "  %%r = call %s %s(%s)\n", lt(fs->ret), fs->ll, args.data ? args.data : "");
        sb_printf(&b, "  %%slot = alloca %s\n  store %s %%r, ptr %%slot\n", lt(fs->ret), lt(fs->ret));
        sb_printf(&b, "  %%s = call ptr @hl_value_text(ptr %s, ptr %%slot)\n  ret ptr %%s\n}\n", desc_of(fs->ret));
    }
    sb_puts(&m_funcs, b.data);
    sb_free(&b);
    sb_free(&args);
}

static char *str_array(const char *sym, strvec *v) {
    if (!v->n) return xstrdup("null");
    strbuf b = {0};
    for (int i = 0; i < v->n; i++) sb_printf(&b, "%sptr %s", i ? ", " : "", strconst(v->v[i]));
    sb_printf(&m_globals, "%s = private constant [%d x ptr] [%s]\n", sym, v->n, b.data);
    sb_free(&b);
    return xstrdup(sym);
}

static char *persona_text(adef *a) {
    strbuf b = {0};
    for (int i = 0; i < a->npersona; i++) {
        if (i) sb_puts(&b, "\n\n");
        sb_puts(&b, a->persona[i].text);
    }
    return b.data ? sb_take(&b) : NULL;
}

/* ---- what the binary carries: skills and the model profile ---------- */

static char *strip_front_matter(char *text) {
    if (strncmp(text, "---\n", 4) != 0) return text;
    char *end = strstr(text + 4, "\n---");
    if (!end) return text;
    end = strchr(end + 4, '\n');
    return end ? end + 1 : text;
}

/* SKILL.md text for each skill, read now so the program needs no skill files. */
static char *embed_skills(adef *a) {
    if (!a->skills.n) return xstrdup("null");
    const char *home = getenv("HOME");
    char *f = xstrdup(a->decl->file ? a->decl->file : ".");
    char *srcdir = xstrdup(dirname(f));
    free(f);
    strbuf list = {0};
    for (int i = 0; i < a->skills.n; i++) {
        const char *n = a->skills.v[i];
        adef *lang = find_agent(n);
        if (lang && lang->is_skill) {
            sb_printf(&list, "%sptr %s", i ? ", " : "", strconst(lang->instructions ? lang->instructions : ""));
            continue;
        }
        char *cands[] = {
            xasprintf("%s/skills/%s/SKILL.md", srcdir, n),
            xasprintf("skills/%s/SKILL.md", n),
            xasprintf(".hola/skills/%s/SKILL.md", n),
            xasprintf("%s/.hola/skills/%s/SKILL.md", home ? home : "", n),
        };
        char *text = NULL;
        for (size_t k = 0; k < sizeof(cands) / sizeof(cands[0]) && !text; k++) {
            FILE *fp = fopen(cands[k], "rb");
            if (!fp) continue;
            strbuf b = {0};
            char buf[4096];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) sb_putn(&b, buf, got);
            fclose(fp);
            text = sb_take(&b);
        }
        if (!text) {
            err(a->decl, "agent %s: skill '%s' not found (looked in %s/skills, ./skills, ./.hola/skills, ~/.hola/skills)",
                a->name, n, srcdir);
            text = xstrdup("");
        }
        sb_printf(&list, "%sptr %s", i ? ", " : "", strconst(strip_front_matter(text)));
        for (size_t k = 0; k < sizeof(cands) / sizeof(cands[0]); k++) free(cands[k]);
    }
    char *sym = xasprintf("%s.skill_texts", a->sym);
    sb_printf(&m_globals, "%s = private constant [%d x ptr] [%s]\n", sym, a->skills.n, list.data);
    sb_free(&list);
    free(srcdir);
    return sym;
}

static cJSON *g_profiles;      /* profiles.json seen at compile time */
static int g_profiles_loaded;

static cJSON *read_json(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    strbuf b = {0};
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) sb_putn(&b, buf, got);
    fclose(fp);
    cJSON *j = cJSON_Parse(b.data ? b.data : "");
    sb_free(&b);
    return j;
}

/* The same files, in the same order, as the core's hola_profile_load. */
static cJSON *profiles(void) {
    if (g_profiles_loaded) return g_profiles;
    g_profiles_loaded = 1;
    g_profiles = read_json(".hola/profiles.json");
    if (!g_profiles && getenv("HOME")) {
        char *p = xasprintf("%s/.hola/profiles.json", getenv("HOME"));
        g_profiles = read_json(p);
        free(p);
    }
    return g_profiles;
}

static int secret_key(const char *k) {
    static const char *const words[] = {"key", "token", "secret", "password", "auth", NULL};
    char low[128];
    size_t n = strlen(k);
    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)k[i]);
    low[n] = '\0';
    for (int i = 0; words[i]; i++)
        if (strstr(low, words[i])) return 1;
    return 0;
}

/* Drops every string whose name looks like a credential, at any depth. */
static void strip_secrets(cJSON *o) {
    for (cJSON *c = o ? o->child : NULL, *next; c; c = next) {
        next = c->next;
        if (cJSON_IsString(c) && c->string && secret_key(c->string)) cJSON_DeleteItemFromObject(o, c->string);
        else if (cJSON_IsObject(c) || cJSON_IsArray(c)) strip_secrets(c);
    }
}

/* The agent's profile settings, so the program runs without profiles.json.
 * The API key is never embedded; the program reads it when it runs. */
static char *embed_profile(adef *a, const char **name_out) {
    cJSON *root = profiles();
    const char *name = a->profile;
    if (!name && root) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "default");
        if (cJSON_IsString(d)) name = d->valuestring;
    }
    *name_out = name;
    if (!name || a->is_abstract) return NULL;
    cJSON *prof = root ? cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "profiles"), name) : NULL;
    if (!cJSON_IsObject(prof)) {
        /* codex and claude are built into the core and sign in locally */
        if (strcmp(name, "codex") != 0 && strcmp(name, "claude") != 0)
            err(a->decl, "agent %s: profile '%s' not found in .hola/profiles.json or ~/.hola/profiles.json", a->name, name);
        return NULL;
    }
    cJSON *copy = cJSON_Duplicate(prof, 1);
    strip_secrets(copy);
    char *s = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return s;
}

static void gen_agent(adef *a) {
    char *skills = str_array(xasprintf("%s.skills", a->sym), &a->skills);
    char *tools = str_array(xasprintf("%s.tools", a->sym), &a->tools);
    char *deny = str_array(xasprintf("%s.deny", a->sym), &a->deny);
    char *custom = xstrdup("null");
    if (a->nftools) {
        strbuf b = {0};
        for (int i = 0; i < a->nftools; i++) {
            fsig *t = a->ftools[i];
            sb_printf(&b, "%s%%hl.tooldef { ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, ptr %s }",
                      i ? ", " : "", strconst(t->name), opt_strconst(t->doc ? t->doc : t->name),
                      struct_desc(t->params_sd), t->ret->k == T_VOID ? "null" : desc_of(t->ret),
                      t->binding ? "null" : t->wrapper, opt_strconst(t->binding), opt_strconst(t->templ),
                      opt_strconst(t->templ2), t->sub ? t->sub->sym : "null");
        }
        free(custom);
        custom = xasprintf("%s.custom", a->sym);
        sb_printf(&m_globals, "%s = private constant [%d x %%hl.tooldef] [%s]\n", custom, a->nftools, b.data);
        sb_free(&b);
    }
    strbuf parents = {0};
    for (int i = 0; i < a->parents.n; i++) sb_printf(&parents, "%s%s", i ? "," : "", a->parents.v[i]);
    char *persona = persona_text(a);
    char *skill_texts = embed_skills(a);
    const char *profile_name = a->profile;
    char *profile_json = embed_profile(a, &profile_name);
    sb_printf(&m_globals,
              "%s = constant %%hl.agentdef { ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, ptr %s, i64 %lld, "
              "i64 %d, ptr %s, i64 %d, ptr %s, i64 %d, ptr %s, ptr %s, ptr %s, i64 %d, ptr %s, i64 %d, ptr %s, "
              "ptr %s, ptr %s }\n",
              a->sym, strconst(a->name), opt_strconst(a->doc), opt_strconst(profile_name), opt_strconst(persona),
              opt_strconst(a->goal), opt_strconst(a->task), (long long)a->max_turns, a->skills.n, skills,
              a->tools.n, tools, a->deny.n, deny, struct_desc(a->input),
              a->output ? struct_desc(a->output) : "null", a->nftools, custom, a->is_abstract,
              strconst(parents.data ? parents.data : ""), skill_texts, opt_strconst(profile_json));
    sb_free(&parents);
}

static const char *k_runtime_decls =
    "%hl.type = type { i64, i64, ptr, ptr, i64, ptr }\n"
    "%hl.field = type { ptr, ptr, i64, ptr }\n"
    "%hl.tooldef = type { ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }\n"
    "%hl.agentdef = type { ptr, ptr, ptr, ptr, ptr, ptr, i64, i64, ptr, i64, ptr, i64, ptr, ptr, ptr, i64, ptr, i64, ptr, ptr, ptr }\n"
    "@hl_ty_int = external constant %hl.type\n"
    "@hl_ty_float = external constant %hl.type\n"
    "@hl_ty_bool = external constant %hl.type\n"
    "@hl_ty_string = external constant %hl.type\n"
    "declare ptr @hl_alloc(i64)\n"
    "declare void @hl_panic_at(ptr, ptr, i64)\n"
    "declare void @hl_check_nil(ptr, ptr, i64)\n"
    "declare i64 @hl_div(i64, i64, ptr, i64)\n"
    "declare i64 @hl_mod(i64, i64, ptr, i64)\n"
    "declare ptr @hl_str_concat(ptr, ptr)\n"
    "declare i64 @hl_str_cmp(ptr, ptr)\n"
    "declare i64 @hl_str_len(ptr)\n"
    "declare i64 @hl_str_at(ptr, i64, ptr, i64)\n"
    "declare ptr @hl_int_to_str(i64)\n"
    "declare ptr @hl_float_to_str(double)\n"
    "declare ptr @hl_bool_to_str(i64)\n"
    "declare i64 @hl_str_to_int(ptr)\n"
    "declare double @hl_str_to_float(ptr)\n"
    "declare i64 @hl_contains(ptr, ptr)\n"
    "declare i64 @hl_has_prefix(ptr, ptr)\n"
    "declare i64 @hl_has_suffix(ptr, ptr)\n"
    "declare i64 @hl_index_of(ptr, ptr)\n"
    "declare ptr @hl_substr(ptr, i64, i64)\n"
    "declare ptr @hl_trim(ptr)\n"
    "declare ptr @hl_upper(ptr)\n"
    "declare ptr @hl_lower(ptr)\n"
    "declare ptr @hl_replace(ptr, ptr, ptr)\n"
    "declare ptr @hl_split(ptr, ptr)\n"
    "declare ptr @hl_join(ptr, ptr)\n"
    "declare ptr @hl_arr_new(i64)\n"
    "declare ptr @hl_arr_grow(ptr, i64)\n"
    "declare ptr @hl_arr_at(ptr, i64, ptr, i64)\n"
    "declare i64 @hl_arr_len(ptr)\n"
    "declare void @hl_print_str(ptr)\n"
    "declare void @hl_print_int(i64)\n"
    "declare void @hl_print_float(double)\n"
    "declare void @hl_print_bool(i64)\n"
    "declare void @hl_print_value(ptr, ptr)\n"
    "declare void @hl_print_space()\n"
    "declare void @hl_print_nl()\n"
    "declare ptr @hl_to_json(ptr, ptr)\n"
    "declare ptr @hl_value_text(ptr, ptr)\n"
    "declare ptr @hl_sh(ptr)\n"
    "declare i64 @hl_exit_code()\n"
    "declare ptr @hl_read_file(ptr)\n"
    "declare i64 @hl_write_file(ptr, ptr)\n"
    "declare i64 @hl_file_exists(ptr)\n"
    "declare ptr @hl_getenv(ptr)\n"
    "declare ptr @hl_args()\n"
    "declare void @hl_exit(i64)\n"
    "declare void @hl_sleep_ms(i64)\n"
    "declare i64 @hl_now_ms()\n"
    "declare ptr @hl_agent_run(ptr, ptr)\n"
    "declare void @hl_agent_cli()\n";

static void reset_tables(void) {
    g_errors = 0;
    g_nstructs = g_nfuncs = g_nagents = g_nstrs = g_nbound = 0;
    sb_free(&m_types);
    sb_free(&m_globals);
    sb_free(&m_funcs);
    sb_free(&m_strtab);
    init_key();
}

gen_result gen_program(nodelist *top, const char *ir_path) {
    gen_result res = {0, 0};
    reset_tables();
    declare_all(top);

    fsig *mainf = find_func("main");
    int runnable = 0;
    for (int i = 0; i < g_nagents; i++) runnable += !g_agents[i]->is_abstract;
    if (!mainf && runnable) {
        /* No main: the program is its agent(s). The runtime turns the command
         * line into the agent's input and runs the loop; the model decides
         * what to do with the tools it has. */
        sb_puts(&m_funcs, "\ndefine void @hl_fn_main() {\nentry:\n  call void @hl_agent_cli()\n  ret void\n}\n");
    } else if (!mainf) {
        fprintf(stderr, "error: the program has no func main() and no agent to run\n");
        g_errors++;
    } else if (mainf->np || mainf->ret->k != T_VOID) {
        err(mainf->decl, "func main must take no arguments and return nothing");
    }

    for (int i = 0; i < g_nstructs; i++) {
        sdef *sd = g_structs[i];
        sb_printf(&m_types, "%s = type { ", struct_ll(sd));
        for (int k = 0; k < sd->nf; k++) sb_printf(&m_types, "%s%s", k ? ", " : "", lt(sd->f[k].t));
        sb_puts(&m_types, " }\n");
    }
    for (int i = 0; i < g_nfuncs; i++) {
        gen_function(g_funcs[i]);
        if (g_funcs[i]->tool_of) gen_tool_wrapper(g_funcs[i]);
    }
    strbuf table = {0};
    int ntable = 0;
    for (int i = 0; i < g_nagents; i++) {
        if (g_agents[i]->is_skill) continue;
        gen_agent(g_agents[i]);
        sb_printf(&table, "%sptr %s", ntable++ ? ", " : "", g_agents[i]->sym);
    }
    if (ntable)
        sb_printf(&m_globals, "@hl_agents = constant [%d x ptr] [%s]\n", ntable, table.data);
    else
        sb_puts(&m_globals, "@hl_agents = constant [0 x ptr] zeroinitializer\n");
    sb_printf(&m_globals, "@hl_nagents = constant i64 %d\n", ntable);
    emit_strtab();
    sb_free(&table);

    res.uses_agents = ntable > 0;
    res.errors = g_errors;
    if (g_errors || !ir_path) return res;

    FILE *out = fopen(ir_path, "w");
    if (!out) { fprintf(stderr, "holacc: cannot write %s\n", ir_path); res.errors = 1; return res; }
    fprintf(out, "; generated by holacc\ntarget triple = \"x86_64-pc-linux-gnu\"\n\n%s\n%s\n%s\n%s",
            k_runtime_decls, m_types.data ? m_types.data : "", m_globals.data ? m_globals.data : "",
            m_funcs.data ? m_funcs.data : "");
    fclose(out);
    return res;
}

/* ================================================================== */
/* holacc agents: the resolved agents as JSON-like manifests           */
/* ================================================================== */

static void json_str(const char *s) {
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c < 32) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void json_type(type *t);

static void json_struct(sdef *sd) {
    printf("{\"type\":\"object\",\"properties\":{");
    for (int i = 0; i < sd->nf; i++) {
        if (i) putchar(',');
        json_str(sd->f[i].name);
        putchar(':');
        json_type(sd->f[i].t);
    }
    printf("}}");
}

static void json_type(type *t) {
    switch (t->k) {
    case T_INT: printf("{\"type\":\"integer\"}"); break;
    case T_FLOAT: printf("{\"type\":\"number\"}"); break;
    case T_BOOL: printf("{\"type\":\"boolean\"}"); break;
    case T_STRING: printf("{\"type\":\"string\"}"); break;
    case T_ARRAY: printf("{\"type\":\"array\",\"items\":"); json_type(t->elem); putchar('}'); break;
    case T_STRUCT: json_struct(t->sd); break;
    default: printf("null");
    }
}

static void json_list(strvec *v) {
    putchar('[');
    for (int i = 0; i < v->n; i++) { if (i) putchar(','); json_str(v->v[i]); }
    putchar(']');
}

int gen_describe_agents(nodelist *top) {
    reset_tables();
    declare_all(top);
    if (g_errors) return 1;
    printf("[");
    int shown = 0;
    for (int i = 0; i < g_nagents; i++) {
        adef *a = g_agents[i];
        if (a->is_skill) continue;
        printf("%s\n {\"name\":", shown++ ? "," : "");
        json_str(a->name);
        printf(",\"abstract\":%s,\"extends\":", a->is_abstract ? "true" : "false");
        json_list(&a->parents);
        if (a->doc) { printf(",\"description\":"); json_str(a->doc); }
        if (a->profile) { printf(",\"profile\":"); json_str(a->profile); }
        char *p = persona_text(a);
        if (p) { printf(",\"persona\":"); json_str(p); }
        if (a->goal) { printf(",\"goal\":"); json_str(a->goal); }
        if (a->task) { printf(",\"task\":"); json_str(a->task); }
        if (a->max_turns) printf(",\"max_turns\":%lld", (long long)a->max_turns);
        printf(",\"skills\":"); json_list(&a->skills);
        printf(",\"tools\":"); json_list(&a->tools);
        printf(",\"deny\":"); json_list(&a->deny);
        printf(",\"functions\":[");
        for (int k = 0; k < a->nftools; k++) { if (k) putchar(','); json_str(a->ftools[k]->name); }
        printf("],\"inputs\":");
        json_struct(a->input);
        if (a->output) { printf(",\"outputs\":"); json_struct(a->output); }
        putchar('}');
    }
    printf("\n]\n");
    return 0;
}
