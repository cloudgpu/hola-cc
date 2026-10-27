/*
 * rt_agent.c — runs the agents of a compiled program through the Hola
 * agent loop (libhola).
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 *
 * The program is self-contained: its agents, prompts, skills and model
 * settings are compiled in, and Hola's coder and git tools are linked in.
 * Only the API key is read when it runs.
 *
 * For each Agent{...}.run() the runtime:
 *   1. builds a provider from the profile compiled into the program (the key
 *      from HOLA_API_KEY or this machine's profile of the same name), or from
 *      HOLA_PROFILE on this machine when that is set;
 *   2. builds a tool registry with the agent's tools: Hola's built-in tools picked
 *      by name or toolset, minus `deny`, plus the functions the agent declares,
 *      plus submit_result when the agent has an output;
 *   3. sends the task (the template filled from the input) and runs the loop;
 *   4. returns the validated submit_result value, or the final text.
 */
#define _GNU_SOURCE
#define HL_RT_INTERNAL
#include "hl_rt.h"

#include "hola/agent.h"
#include "hola/agent_callbacks.h"
#include "hola/agent_config.h"
#include "hola/builtin_tools.h"
#include "hola/plugin.h"
#include "hola/profile.h"
#include "hola/provider.h"
#include "hola/session_db.h"
#include "hola/skills.h"
#include "hola/tool.h"
#include "hola/tool_registry.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S(x) ((x) ? (x) : "")

/* One running agent. Runs nest when a tool function runs another agent. */
typedef struct run_ctx {
    const hl_agentdef *def;
    void *result;            /* the decoded output struct, once submitted */
    int have_result;
    char *last_error;
    struct run_ctx *outer;
} run_ctx;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static run_ctx *g_top;
static hola_tool_registry_t g_catalog;
static int g_catalog_ready;

static int verbose(void) {
    const char *v = getenv("HOLACC_VERBOSE");
    return v && *v && strcmp(v, "0") != 0;
}

static void fail(const hl_agentdef *def, const char *what) __attribute__((noreturn));
static void fail(const hl_agentdef *def, const char *what) {
    fflush(stdout);
    fprintf(stderr, "agent %s: %s\n", def ? def->name : "?", S(what));
    exit(3);
}

/* ---- the plugin tool catalog --------------------------------------- */

/* coder_tools and git come compiled from the ADK (libhola_tools.a), so the
 * program needs no plugin files. HOLA_PLUGIN_DIR opts in to loading more: the
 * plugins named in HOLACC_PLUGINS (comma-separated) from that directory. */

static void catalog_init(void) {
    if (g_catalog_ready) return;
    g_catalog_ready = 1;
    hola_tool_registry_init(&g_catalog);
    hola_plugin_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    hl_builtin_coder_tools(&g_catalog, &desc);
    memset(&desc, 0, sizeof(desc));
    hl_builtin_git(&g_catalog, &desc);

    const char *dir = getenv("HOLA_PLUGIN_DIR");
    const char *names = getenv("HOLACC_PLUGINS");
    if (!dir || !*dir || !names || !*names) return;
    char *list = strdup(names);
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s.so", dir, tok);
        if (!hola_plugin_load(path, &g_catalog))
            fprintf(stderr, "holacc: plugin %s: %s\n", path, S(hola_plugin_last_error()));
    }
    free(list);
}

static int listed(const char *const *list, int64_t n, const char *name, const char *toolset) {
    for (int64_t i = 0; i < n; i++) {
        if (strcmp(list[i], name) == 0) return 1;
        if (toolset && strcmp(list[i], toolset) == 0) return 1;
    }
    return 0;
}

static char *field_text(const hl_type *in, const void *base, const char *name);

/* ---- tools written in the language --------------------------------- */

static const hl_tooldef *find_custom(const char *name) {
    for (run_ctx *c = g_top; c; c = c->outer)
        for (int64_t i = 0; i < c->def->ncustom; i++)
            if (strcmp(c->def->custom[i].name, name) == 0) return &c->def->custom[i];
    return NULL;
}

/* ---- declarative tools: sh, http, print, read, write, agent ---------- */

#define MAX_TOOL_OUTPUT (64 * 1024)

typedef enum { ESC_RAW, ESC_SHELL, ESC_URL } esc_mode;

static void put_escaped(FILE *m, const char *v, esc_mode mode) {
    if (mode == ESC_SHELL) {
        /* one shell word, whatever the model sent: 'it'\''s' */
        fputc('\'', m);
        for (const char *p = v; *p; p++) {
            if (*p == '\'') fputs("'\\''", m);
            else fputc(*p, m);
        }
        fputc('\'', m);
    } else if (mode == ESC_URL) {
        for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                *p == '-' || *p == '_' || *p == '.' || *p == '~')
                fputc(*p, m);
            else
                fprintf(m, "%%%02X", *p);
        }
    } else {
        fputs(v, m);
    }
}

/* Fills {{param}} from the decoded parameters. */
static char *render(const char *templ, const hl_type *pt, const void *params, esc_mode mode) {
    char *out = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&out, &len);
    for (const char *p = S(templ); *p;) {
        const char *open = strstr(p, "{{");
        const char *close = open ? strstr(open + 2, "}}") : NULL;
        if (!open || !close) { fputs(p, m); break; }
        fwrite(p, 1, (size_t)(open - p), m);
        char key[256];
        size_t kn = (size_t)(close - open - 2);
        if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, open + 2, kn);
        key[kn] = '\0';
        char *k = key;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' ';) *--e = '\0';
        char *v = field_text(pt, params, k);
        put_escaped(m, v ? v : "", mode);
        free(v);
        p = close + 2;
    }
    fclose(m);
    return out;
}

static char *clip(char *s, size_t n) {
    if (n > MAX_TOOL_OUTPUT) {
        s[MAX_TOOL_OUTPUT] = '\0';
        char *c = NULL;
        if (asprintf(&c, "%s\n[output cut at %d bytes of %zu]", s, MAX_TOOL_OUTPUT, n) >= 0) { free(s); return c; }
    }
    return s;
}

/* Files the model names stay inside the directory the program runs in. */
static int safe_path(const char *p) {
    if (!*p || p[0] == '/') return 0;
    for (const char *s = p; (s = strstr(s, "..")); s += 2)
        if ((s == p || s[-1] == '/') && (s[2] == '\0' || s[2] == '/')) return 0;
    return 1;
}

static hola_tool_result_t *text_result(char *text, int ok) {
    hola_tool_result_t *r = ok ? hola_tool_result_new_success(S(text)) : hola_tool_result_new_error(S(text), 0);
    free(text);
    return r;
}

static char *read_stream(FILE *f, size_t *n_out) {
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) fwrite(chunk, 1, n, m);
    fclose(m);
    *n_out = len;
    return buf;
}

static hola_tool_result_t *run_binding(const hl_tooldef *t, void *params) {
    const char *b = t->binding;
    if (strcmp(b, "print") == 0) {
        char *text = render(t->templ, t->params, params, ESC_RAW);
        printf("%s\n", text);
        fflush(stdout);
        free(text);
        return hola_tool_result_new_success("shown to the user");
    }
    if (strcmp(b, "sh") == 0 || strcmp(b, "http") == 0) {
        char *cmd;
        if (b[0] == 's') {
            char *body = render(t->templ, t->params, params, ESC_SHELL);
            if (asprintf(&cmd, "( %s ) 2>&1", body) < 0) cmd = NULL;
            free(body);
        } else {
            char *url = render(t->templ, t->params, params, ESC_URL);
            /* the URL is one shell word; curl reports HTTP errors in its output */
            char *q = NULL;
            size_t ql = 0;
            FILE *m = open_memstream(&q, &ql);
            put_escaped(m, url, ESC_SHELL);
            fclose(m);
            if (asprintf(&cmd, "curl -sS -L --max-time 30 -w '\\n[HTTP %%{http_code}]' %s 2>&1", q) < 0) cmd = NULL;
            free(q);
            free(url);
        }
        if (verbose()) fprintf(stderr, "    $ %s\n", cmd);
        fflush(stdout);
        FILE *p = cmd ? popen(cmd, "r") : NULL;
        free(cmd);
        if (!p) return hola_tool_result_new_error("Error: could not run the command", 0);
        size_t n;
        char *out = read_stream(p, &n);
        int st = pclose(p);
        out = clip(out, n);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return text_result(out, 1);
        char *msg = NULL;
        if (asprintf(&msg, "exit status %d\n%s", WIFEXITED(st) ? WEXITSTATUS(st) : -1, out) < 0) msg = NULL;
        free(out);
        return text_result(msg, 0);
    }
    if (strcmp(b, "read") == 0 || strcmp(b, "write") == 0) {
        char *path = render(t->templ, t->params, params, ESC_RAW);
        if (!safe_path(path)) {
            free(path);
            return hola_tool_result_new_error("Error: the path must be relative and stay inside the working directory", 0);
        }
        if (b[0] == 'r') {
            FILE *f = fopen(path, "rb");
            free(path);
            if (!f) return hola_tool_result_new_error("Error: cannot read the file", 0);
            size_t n;
            char *data = read_stream(f, &n);
            fclose(f);
            return text_result(clip(data, n), 1);
        }
        char *content = render(t->templ2, t->params, params, ESC_RAW);
        FILE *f = fopen(path, "wb");
        int ok = f && fputs(content, f) >= 0;
        if (f && fclose(f) != 0) ok = 0;
        char *msg = NULL;
        int rc = ok ? asprintf(&msg, "wrote %zu bytes to %s", strlen(content), path)
                    : asprintf(&msg, "Error: cannot write %s", path);
        if (rc < 0) msg = NULL;
        free(content);
        free(path);
        return text_result(msg, ok);
    }
    if (strcmp(b, "agent") == 0 && t->sub) {
        void *out = hl_agent_run(t->sub, params);
        const char *text = t->sub->output ? hl_to_json(t->sub->output, &out) : (const char *)out;
        return hola_tool_result_new_success(S(text));
    }
    return hola_tool_result_new_error("Error: unknown tool action", 0);
}

static hola_tool_result_t *custom_handler(const char *name, const cJSON *args, const char *task_id, void *ud) {
    (void)task_id; (void)ud;
    pthread_mutex_lock(&g_lock);
    const hl_tooldef *t = find_custom(name);
    pthread_mutex_unlock(&g_lock);
    if (!t) return hola_tool_result_new_error("Error: unknown tool", 0);
    void *params = NULL;
    char *err = NULL;
    cJSON *empty = NULL;
    if (!args) args = empty = cJSON_CreateObject();
    int rc = hl_value_from_cjson(t->params, args, &params, "", &err);
    cJSON_Delete(empty);
    if (rc != 0) {
        char *msg = NULL;
        if (asprintf(&msg, "Error: invalid arguments for %s: %s", name, S(err)) < 0) msg = NULL;
        hola_tool_result_t *r = hola_tool_result_new_error(msg ? msg : "Error: invalid arguments", 0);
        free(msg);
        free(err);
        return r;
    }
    if (t->binding) return run_binding(t, params);
    const char *out = t->wrapper(params);
    return hola_tool_result_new_success(S(out));
}

static hola_tool_result_t *submit_handler(const char *name, const cJSON *args, const char *task_id, void *ud) {
    (void)name; (void)task_id; (void)ud;
    pthread_mutex_lock(&g_lock);
    run_ctx *c = g_top;
    pthread_mutex_unlock(&g_lock);
    if (!c || !c->def->output) return hola_tool_result_new_error("Error: nothing to submit", 0);
    void *value = NULL;
    char *err = NULL;
    if (!args || hl_value_from_cjson(c->def->output, args, &value, "", &err) != 0) {
        free(c->last_error);
        c->last_error = err ? err : strdup("missing arguments");
        char *msg = NULL;
        if (asprintf(&msg, "Error: the result does not match the output schema: %s. Fix it and call submit_result again.",
                     c->last_error) < 0) msg = NULL;
        hola_tool_result_t *r = hola_tool_result_new_error(msg ? msg : "Error: invalid result", 0);
        free(msg);
        return r;
    }
    c->result = value;
    c->have_result = 1;
    return hola_tool_result_new_success("Result accepted. Reply with one short line saying you are done.");
}

static bool check_always(void) { return true; }

static int register_schema_tool(hola_tool_registry_t *reg, const char *name, const char *desc, cJSON *params,
                                hola_tool_handler_fn fn, hola_tool_effect_t effect) {
    hola_tool_schema_t schema = {.name = (char *)name, .description = (char *)desc, .parameters = params};
    hola_tool_metadata_t meta = HOLA_TOOL_META(effect, HOLA_TOOL_RISK_LOW, true, false, false);
    int rc = hola_tool_registry_register(reg, name, "holacc", &schema, &meta, fn, check_always, false, "λ", 0);
    cJSON_Delete(params);
    return rc;
}

static hola_tool_registry_t *build_registry(const hl_agentdef *def) {
    hola_tool_registry_t *reg = calloc(1, sizeof(*reg));
    if (!reg) fail(def, "out of memory");
    hola_tool_registry_init(reg);
    if (def->ntools) {
        catalog_init();
        for (const hola_tool_entry_t *e = g_catalog.head; e; e = e->next) {
            if (!listed(def->tools, def->ntools, e->name, e->toolset)) continue;
            if (listed(def->deny, def->ndeny, e->name, e->toolset)) continue;
            hola_tool_registry_register(reg, e->name, e->toolset, e->schema, &e->metadata, e->handler,
                                        e->check_fn, e->requires_env, e->emoji, e->max_result_size_chars);
        }
        for (int64_t i = 0; i < def->ntools; i++) {
            int found = 0;
            for (const hola_tool_entry_t *e = g_catalog.head; e && !found; e = e->next)
                found = strcmp(e->name, def->tools[i]) == 0 || (e->toolset && strcmp(e->toolset, def->tools[i]) == 0);
            if (!found)
                fprintf(stderr, "agent %s: warning: no loaded plugin provides tool or toolset '%s'\n",
                        def->name, def->tools[i]);
        }
    }
    for (int64_t i = 0; i < def->ncustom; i++) {
        const hl_tooldef *t = &def->custom[i];
        if (listed(def->deny, def->ndeny, t->name, NULL)) continue;
        register_schema_tool(reg, t->name, S(t->desc), hl_type_schema(t->params, NULL), custom_handler,
                             HOLA_TOOL_EFFECT_UNKNOWN);
    }
    if (def->output)
        register_schema_tool(reg, "submit_result",
                             "Submit the final result of the task. Call this exactly once, when the work is done, "
                             "with every field filled in.",
                             hl_type_schema(def->output, NULL), submit_handler, HOLA_TOOL_EFFECT_SUCCESS_NO_CHANGE);
    return reg;
}

/* ---- provider ------------------------------------------------------ */

static void set_str(char **dst, const char *v) {
    if (!v || !*v) return;
    free(*dst);
    *dst = strdup(v);
}

/* The API key for an embedded profile: HOLA_API_KEY, else the key of the
 * profile with the same name in this machine's profiles.json, else none. */
static char *runtime_key(const hl_agentdef *def, const char *cwd) {
    const char *env = getenv("HOLA_API_KEY");
    if (env && *env) return strdup(env);
    hola_profile_t live = {0};
    char *key = NULL;
    if (hola_profile_load(def->profile, cwd, &live) == 0 && live.api_key) key = strdup(live.api_key);
    hola_profile_free(&live);
    return key;
}

/* Applies the profile compiled into the program. hola_profile_load reads a
 * project's .hola/profiles.json first, so the embedded settings (plus the key)
 * go into a private temporary project for the moment of loading. */
static void apply_embedded_profile(const hl_agentdef *def, hola_agent_config_t *cfg, const char *cwd) {
    cJSON *prof = cJSON_Parse(def->profile_json);
    if (!prof) fail(def, "the embedded profile is not valid JSON");
    char *key = runtime_key(def, cwd);
    if (key) cJSON_AddStringToObject(prof, "api_key", key);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(cJSON_AddObjectToObject(root, "profiles"), def->profile, prof);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    const char *tmp = getenv("TMPDIR");
    char dir[4096], hola_dir[4200], file[4300];
    snprintf(dir, sizeof(dir), "%s/holacc-profile-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) fail(def, "cannot create a temporary directory for the profile");
    snprintf(hola_dir, sizeof(hola_dir), "%s/.hola", dir);
    snprintf(file, sizeof(file), "%s/profiles.json", hola_dir);
    int ok = 0;
    if (mkdir(hola_dir, 0700) == 0) {
        int fd = open(file, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            size_t n = strlen(text);
            ok = write(fd, text, n) == (ssize_t)n;
            close(fd);
        }
    }
    hola_profile_t p = {0};
    int rc = ok ? hola_profile_load(def->profile, dir, &p) : -1;
    unlink(file);
    rmdir(hola_dir);
    rmdir(dir);
    free(text);
    if (rc != 0) fail(def, "could not apply the embedded profile");
    hola_profile_apply_to_config_ex(&p, cfg, false);
    hola_profile_free(&p);
    free(cfg->api_key);
    cfg->api_key = key;      /* exactly the run-time key, or none */
}

static hola_agent_config_t *make_config(const hl_agentdef *def) {
    hola_agent_config_t *cfg = hola_agent_config_default();
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
    const char *live = getenv("HOLA_PROFILE");
    hola_profile_t prof = {0};
    int from_env = 0;
    if (live && *live) {
        /* HOLA_PROFILE swaps in a profile from this machine instead */
        if (hola_profile_load(live, cwd, &prof) != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "profile '%s' (HOLA_PROFILE) not found in ~/.hola/profiles.json", live);
            fail(def, msg);
        }
        hola_profile_apply_to_config(&prof, cfg);
        hola_profile_free(&prof);
        from_env = 1;
    } else if (def->profile_json) {
        apply_embedded_profile(def, cfg, cwd);
    } else if (def->profile && *def->profile) {
        /* codex / claude: built into the core, signed in on this machine */
        if (hola_profile_load(def->profile, cwd, &prof) != 0) fail(def, "profile not available");
        hola_profile_apply_to_config(&prof, cfg);
        hola_profile_free(&prof);
        from_env = 1;
    } else if (getenv("OPENAI_BASE_URL") || getenv("OPENAI_API_KEY") || getenv("OLLAMA_BASE_URL")) {
        from_env = 1;   /* compiled without a profile: the environment configures the model */
    } else {
        fail(def, "no model configured: the program was compiled without a profile; set HOLA_PROFILE, "
                  "or OPENAI_BASE_URL, OPENAI_API_KEY and HOLA_MODEL");
    }
    if (from_env) {
        /* As hola-coder does: the endpoint and key come from the environment
         * the profile exported. No key means no Authorization header. */
        const char *key = getenv("OPENAI_API_KEY");
        free(cfg->api_key);
        cfg->api_key = key ? strdup(key) : NULL;
        set_str(&cfg->base_url, getenv("OPENAI_BASE_URL") ? getenv("OPENAI_BASE_URL") : getenv("OLLAMA_BASE_URL"));
        set_str(&cfg->model, getenv("HOLA_MODEL") ? getenv("HOLA_MODEL")
                             : getenv("OPENAI_MODEL") ? getenv("OPENAI_MODEL") : getenv("OLLAMA_MODEL"));
    }
    if (def->max_turns > 0) cfg->max_turns = (unsigned)def->max_turns;
    cfg->tools_enabled = true;
    /* the program gets the result, not a live stream; HOLACC_STREAM=1 turns it on */
    cfg->streaming = getenv("HOLACC_STREAM") && strcmp(getenv("HOLACC_STREAM"), "1") == 0;
    set_str(&cfg->session_source, "holacc");
    return cfg;
}

/* ---- prompt -------------------------------------------------------- */

static char *system_prompt(const hl_agentdef *def) {
    char *buf = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&buf, &len);
    if (!m) fail(def, "out of memory");
    if (def->persona && *def->persona) fprintf(m, "%s\n", def->persona);
    else fprintf(m, "You are %s, an AI agent.\n", def->name);
    if (def->goal && *def->goal) fprintf(m, "\n## Goal\n%s\n", def->goal);
    for (int64_t i = 0; i < def->nskills; i++)
        fprintf(m, "\n## Skill: %s\n%s\n", def->skills[i], def->skill_texts ? S(def->skill_texts[i]) : "");
    if (def->output)
        fprintf(m, "\n## Result\nWhen the work is done, call the `submit_result` tool exactly once with the result. "
                   "Do not write the result as plain text.\n");
    fclose(m);
    return buf;
}

static char *field_text(const hl_type *in, const void *base, const char *name) {
    for (int64_t i = 0; i < in->nfields; i++) {
        if (strcmp(in->fields[i].name, name) != 0) continue;
        return strdup(hl_value_text(in->fields[i].type, (const char *)base + in->fields[i].offset));
    }
    return NULL;
}

/* The first message: the task template with {{field}} filled in, or the input itself. */
static char *user_message(const hl_agentdef *def, void *input) {
    const hl_type *in = def->input;
    const char *json = hl_to_json(in, &input);
    if (!def->task || !*def->task) {
        if (in->nfields == 1 && in->fields[0].type->kind == HL_K_STRING)
            return field_text(in, input, in->fields[0].name);
        char *m = NULL;
        if (asprintf(&m, "Input:\n```json\n%s\n```", json) < 0) fail(def, "out of memory");
        return m;
    }
    char *out = NULL;
    size_t len = 0;
    FILE *m = open_memstream(&out, &len);
    for (const char *p = def->task; *p;) {
        const char *open = strstr(p, "{{");
        const char *close = open ? strstr(open + 2, "}}") : NULL;
        if (!open || !close) { fputs(p, m); break; }
        fwrite(p, 1, (size_t)(open - p), m);
        char key[256];
        size_t kn = (size_t)(close - open - 2);
        if (kn >= sizeof(key)) kn = sizeof(key) - 1;
        memcpy(key, open + 2, kn);
        key[kn] = '\0';
        char *k = key;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && e[-1] == ' ';) *--e = '\0';
        char *v = strcmp(k, "input") == 0 ? strdup(json) : field_text(in, input, k);
        if (v) fputs(v, m);
        else fwrite(open, 1, (size_t)(close + 2 - open), m);
        free(v);
        p = close + 2;
    }
    fclose(m);
    return out;
}

/* ---- callbacks ----------------------------------------------------- */

static void on_tool_start(const char *name, const char *args, void *ud) {
    if (!verbose()) return;
    const hl_agentdef *def = ud;
    fprintf(stderr, "  [%s] %s(%.200s)\n", def->name, name, S(args));
}

/* ---- run ----------------------------------------------------------- */

void *hl_agent_run(const hl_agentdef *def, void *input) {
    if (def->is_abstract) fail(def, "is abstract and cannot run");
    hola_agent_config_t *cfg = make_config(def);
    hola_provider_t *provider = hola_provider_create(cfg->provider_name, cfg->model, cfg->base_url,
                                                     cfg->api_key, cfg->api_mode);
    if (!provider) fail(def, "could not create the model provider");
    hola_tool_registry_t *reg = build_registry(def);
    const char *dbp = getenv("HOLACC_SESSION_DB");
    hola_session_db_t *db = hola_session_db_open(dbp && *dbp ? dbp : ":memory:");
    hola_agent_t *agent = hola_agent_create(cfg, reg, provider, db);
    if (!agent) fail(def, "could not create the agent");
    char *sys = system_prompt(def);
    hola_agent_set_system_prompt(agent, sys);
    char *msg = user_message(def, input);

    run_ctx ctx = {.def = def};
    pthread_mutex_lock(&g_lock);
    ctx.outer = g_top;
    g_top = &ctx;
    pthread_mutex_unlock(&g_lock);

    if (verbose())
        fprintf(stderr, "[%s] running (provider %s, model %s, %s, %s)\n", def->name, S(cfg->provider_name),
                S(cfg->model), S(cfg->base_url), cfg->api_key ? "with API key" : "no API key");
    hola_agent_callbacks_t cb = {.on_tool_start = on_tool_start};
    char *text = NULL;
    int rc = hola_agent_run(agent, msg, &cb, (void *)def, &text);
    for (int tries = 0; rc != -1 && def->output && !ctx.have_result && tries < 2; tries++) {
        free(text);
        text = NULL;
        rc = hola_agent_run(agent,
                            "You have not called submit_result yet. Call submit_result now with the final result.",
                            &cb, (void *)def, &text);
    }

    pthread_mutex_lock(&g_lock);
    g_top = ctx.outer;
    pthread_mutex_unlock(&g_lock);

    if (rc == -1) {
        char m[1024];
        snprintf(m, sizeof(m), "the model call failed: %s", text && *text ? text : "unknown error");
        fail(def, m);
    }
    void *ret;
    if (def->output) {
        if (!ctx.have_result) {
            char m[1024];
            snprintf(m, sizeof(m), "finished without a valid result%s%s", ctx.last_error ? ": " : "",
                     S(ctx.last_error));
            fail(def, m);
        }
        ret = ctx.result;
    } else {
        ret = strdup(S(text));
    }
    free(text);
    free(msg);
    free(sys);
    free(ctx.last_error);
    hola_agent_destroy(agent);
    hola_session_db_close(db);
    hola_tool_registry_destroy(reg);
    free(reg);
    hola_provider_destroy(provider);
    hola_agent_config_free(cfg);
    return ret;
}

/* ---- programs without main: the binary is the agent ----------------- */

static void cli_usage(const hl_agentdef *const *agents, int64_t n, int several) {
    fprintf(stderr, "usage:\n");
    for (int64_t i = 0; i < n; i++) {
        const hl_agentdef *d = agents[i];
        if (d->is_abstract) continue;
        fprintf(stderr, "  <program>%s", several ? " " : "");
        if (several) fputs(d->name, stderr);
        const hl_type *in = d->input;
        if (in->nfields == 1 && in->fields[0].type->kind == HL_K_STRING) {
            fprintf(stderr, " <%s...>", in->fields[0].name);
        } else {
            for (int64_t k = 0; k < in->nfields; k++) fprintf(stderr, " %s=<%s>", in->fields[k].name, in->fields[k].type->name);
        }
        if (d->doc) fprintf(stderr, "\n      %s", d->doc);
        fputc('\n', stderr);
    }
    exit(2);
}

/* "field=value" -> a JSON value of the field's type. */
static cJSON *cli_value(const hl_type *t, const char *v) {
    switch (t->kind) {
    case HL_K_STRING: return cJSON_CreateString(v);
    case HL_K_INT: case HL_K_FLOAT: {
        char *end = NULL;
        double d = strtod(v, &end);
        return end && *end == '\0' && end != v ? cJSON_CreateNumber(d) : cJSON_CreateString(v);
    }
    case HL_K_BOOL:
        if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0) return cJSON_CreateTrue();
        if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0 || strcmp(v, "no") == 0) return cJSON_CreateFalse();
        return cJSON_CreateString(v);
    default: {
        cJSON *j = cJSON_Parse(v);
        return j ? j : cJSON_CreateString(v);
    }
    }
}

void hl_agent_cli(void) {
    hl_arr *argv = hl_args();
    int64_t argc = hl_arr_len(argv);
    const char **av = (const char **)argv->data;
    const hl_agentdef *def = NULL;
    int64_t runnable = 0;
    for (int64_t i = 0; i < hl_nagents; i++)
        if (!hl_agents[i]->is_abstract) { runnable++; def = hl_agents[i]; }
    int64_t first = 0;
    if (runnable > 1) {
        /* several agents: the first argument picks one */
        def = NULL;
        for (int64_t i = 0; argc > 0 && i < hl_nagents; i++)
            if (!hl_agents[i]->is_abstract && strcmp(hl_agents[i]->name, av[0]) == 0) def = hl_agents[i];
        if (!def) cli_usage(hl_agents, hl_nagents, 1);
        first = 1;
    }
    if (argc > first && (strcmp(av[first], "-h") == 0 || strcmp(av[first], "--help") == 0))
        cli_usage(&def, 1, runnable > 1);

    const hl_type *in = def->input;
    cJSON *obj = cJSON_CreateObject();
    if (in->nfields == 1 && in->fields[0].type->kind == HL_K_STRING) {
        /* one text input: all the arguments are the text (or stdin when there are none) */
        size_t len = 0;
        char *text = NULL;
        FILE *m = open_memstream(&text, &len);
        for (int64_t i = first; i < argc; i++) fprintf(m, "%s%s", i > first ? " " : "", av[i]);
        if (argc <= first && !isatty(0)) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) fwrite(buf, 1, n, m);
        }
        fclose(m);
        if (!len) cli_usage(&def, 1, runnable > 1);
        cJSON_AddStringToObject(obj, in->fields[0].name, text);
        free(text);
    } else {
        for (int64_t i = first; i < argc; i++) {
            const char *eq = strchr(av[i], '=');
            const hl_field *f = NULL;
            for (int64_t k = 0; eq && k < in->nfields; k++)
                if (strlen(in->fields[k].name) == (size_t)(eq - av[i]) &&
                    strncmp(in->fields[k].name, av[i], (size_t)(eq - av[i])) == 0) f = &in->fields[k];
            if (!f) { fprintf(stderr, "unknown argument: %s\n", av[i]); cli_usage(&def, 1, runnable > 1); }
            cJSON_DeleteItemFromObject(obj, f->name);
            cJSON_AddItemToObject(obj, f->name, cli_value(f->type, eq + 1));
        }
    }
    void *input = NULL;
    char *err = NULL;
    if (hl_value_from_cjson(in, obj, &input, "", &err) != 0) {
        fprintf(stderr, "%s: input %s\n", def->name, S(err));
        cli_usage(&def, 1, runnable > 1);
    }
    cJSON_Delete(obj);
    void *out = hl_agent_run(def, input);
    if (def->output) puts(hl_to_json(def->output, &out));
    else puts(S((const char *)out));
}

/* ---- --hola-agents ------------------------------------------------- */

void hl_agents_describe(void) {
    cJSON *all = cJSON_CreateArray();
    for (int64_t i = 0; i < hl_nagents; i++) {
        const hl_agentdef *d = hl_agents[i];
        if (d->is_abstract) continue;     /* building blocks, not part of the interface */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", d->name);
        /* The interface only: what the agent takes and returns and which tools
         * it has. Persona, goal, task, skills and profile stay inside. */
        if (d->doc) cJSON_AddStringToObject(o, "description", d->doc);
        cJSON *fns = cJSON_AddArrayToObject(o, "tools");
        for (int64_t k = 0; k < d->ncustom; k++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", d->custom[k].name);
            if (d->custom[k].desc) cJSON_AddStringToObject(t, "description", d->custom[k].desc);
            cJSON_AddItemToObject(t, "parameters", hl_type_schema(d->custom[k].params, NULL));
            cJSON_AddItemToArray(fns, t);
        }
        cJSON_AddItemToObject(o, "inputs", hl_type_schema(d->input, NULL));
        if (d->output) cJSON_AddItemToObject(o, "outputs", hl_type_schema(d->output, NULL));
        cJSON_AddItemToArray(all, o);
    }
    char *s = cJSON_Print(all);
    puts(s ? s : "[]");
    free(s);
    cJSON_Delete(all);
}
