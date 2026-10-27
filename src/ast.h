/*
 * ast.h — syntax tree of the Hola agent language.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#ifndef HOLACC_AST_H
#define HOLACC_AST_H

#include <stddef.h>
#include <stdint.h>

typedef struct node node;

/* A growable list of nodes. */
typedef struct {
    node **items;
    size_t len, cap;
} nodelist;

typedef enum {
    /* types */
    N_TNAME,        /* name (int, string, Foo) or qualified Agent.Input */
    N_TARRAY,       /* []elem */

    /* top level */
    N_FUNC,         /* name, params, ret, body */
    N_PARAM,        /* name, type, text (description) */
    N_STRUCT,       /* name, list = fields (N_PARAM) */
    N_AGENT,        /* name, list = items, list2 = parents (N_TNAME), flag = 1 abstract, 2 skill */
    N_AITEM,        /* agent item: name = key, a = value, flag = override */
    N_IMPORT,       /* text = path */

    /* statements */
    N_BLOCK,        /* list */
    N_VAR,          /* name, type (opt), a = init (opt) */
    N_DEFINE,       /* name := a */
    N_ASSIGN,       /* a = b, op in ival ('=' or '+' '-' '*' '/') */
    N_INCDEC,       /* a ++ / --, ival = +1/-1 */
    N_EXPRSTMT,     /* a */
    N_IF,           /* a = cond, b = then, c = else (block or if) */
    N_FOR,          /* a = init, b = cond, c = post, d = body */
    N_RANGE,        /* name = key var, text = value var, a = expr, d = body */
    N_RETURN,       /* a (opt) */
    N_BREAK,
    N_CONTINUE,

    /* expressions */
    N_INT, N_FLOAT, N_STR, N_BOOL,
    N_IDENT,        /* name */
    N_BINARY,       /* ival = op, a, b */
    N_UNARY,        /* ival = op, a */
    N_CALL,         /* a = callee, list = args */
    N_FIELD,        /* a . name */
    N_INDEX,        /* a [ b ] */
    N_COMPOSITE,    /* type = struct/agent type, list = N_KV */
    N_ARRAYLIT,     /* type = elem type, list = elements */
    N_KV,           /* name: a */
    N_STRLIST,      /* list of N_STR, used in agent items */
    N_FIELDS,       /* list of N_PARAM, used by input/output agent items */
    N_TOOLDEF,      /* agent tool: name, list = params, type = ret, text = desc, a = body */
    N_TOOLBIND      /* declarative tool: name, list = params, text = desc, a = binding (N_IDENT:
                       sh http print read write agent), b, c = templates (N_STR), d = agent (N_IDENT) */
} nkind;

/* Binary operators beyond single characters. */
enum { OP_EQ = 256, OP_NE, OP_LE, OP_GE, OP_AND, OP_OR };

struct node {
    nkind kind;
    int line;
    const char *file;
    char *name;
    char *text;
    int64_t ival;
    double fval;
    int flag;
    node *type;
    node *a, *b, *c, *d;
    nodelist list;
    nodelist list2;
};

node *node_new(nkind k, int line);
void nodelist_push(nodelist *l, node *n);

/* Set by the lexer: the file and line being read. */
extern const char *g_cur_file;
extern int g_cur_line;

/* Parses one file; appends its top-level nodes to `out`. Returns 0 on success. */
int parse_file(const char *path, nodelist *out);
/* Parses source text (used for the built-in prelude). */
int parse_string(const char *name, const char *src, nodelist *out);

#endif
