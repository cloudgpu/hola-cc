/*
 * rt_core.c — core runtime for programs compiled with holacc: strings,
 * arrays, printing, JSON values, processes and files.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 *
 * Memory is never freed: programs are short-lived agent scripts and the
 * runtime has no collector yet. Strings are immutable and NULL means "".
 */
#define _GNU_SOURCE
#define HL_RT_INTERNAL
#include "hl_rt.h"
#include "chacha20.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define S(x) ((x) ? (x) : "")

const hl_type hl_ty_int = {HL_K_INT, 8, "int", NULL, 0, NULL};
const hl_type hl_ty_float = {HL_K_FLOAT, 8, "float", NULL, 0, NULL};
const hl_type hl_ty_bool = {HL_K_BOOL, 1, "bool", NULL, 0, NULL};
const hl_type hl_ty_string = {HL_K_STRING, 8, "string", NULL, 0, NULL};

static int g_argc;
static char **g_argv;
static int64_t g_exit_code;

/* ---- core ---------------------------------------------------------- */

void *hl_alloc(int64_t size) {
    void *p = calloc(1, size > 0 ? (size_t)size : 1);
    if (!p) hl_panic("out of memory");
    return p;
}

static char *dup_n(const char *s, size_t n) {
    char *d = hl_alloc((int64_t)n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *dup_s(const char *s) { return dup_n(S(s), strlen(S(s))); }

void hl_panic(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "panic: %s\n", S(msg));
    exit(2);
}

void hl_panic_at(const char *msg, const char *file, int64_t line) {
    fflush(stdout);
    fprintf(stderr, "%s:%lld: panic: %s\n", S(file), (long long)line, S(msg));
    exit(2);
}

void hl_check_nil(const void *p, const char *file, int64_t line) {
    if (!p) hl_panic_at("use of a nil struct (declare it with a composite literal, e.g. T{})", file, line);
}

int64_t hl_div(int64_t a, int64_t b, const char *file, int64_t line) {
    if (b == 0) hl_panic_at("integer division by zero", file, line);
    return a / b;
}

int64_t hl_mod(int64_t a, int64_t b, const char *file, int64_t line) {
    if (b == 0) hl_panic_at("integer division by zero", file, line);
    return a % b;
}

/* ---- strings ------------------------------------------------------- */

const char *hl_str_concat(const char *a, const char *b) {
    size_t la = strlen(S(a)), lb = strlen(S(b));
    char *d = hl_alloc((int64_t)(la + lb + 1));
    memcpy(d, S(a), la);
    memcpy(d + la, S(b), lb);
    return d;
}

int64_t hl_str_eq(const char *a, const char *b) { return strcmp(S(a), S(b)) == 0; }
int64_t hl_str_cmp(const char *a, const char *b) { return strcmp(S(a), S(b)); }
int64_t hl_str_len(const char *s) { return (int64_t)strlen(S(s)); }

int64_t hl_str_at(const char *s, int64_t i, const char *file, int64_t line) {
    int64_t n = hl_str_len(s);
    if (i < 0 || i >= n) {
        char msg[96];
        snprintf(msg, sizeof(msg), "string index %lld out of range [0:%lld]", (long long)i, (long long)n);
        hl_panic_at(msg, file, line);
    }
    return (unsigned char)s[i];
}

const char *hl_int_to_str(int64_t v) {
    char b[32];
    snprintf(b, sizeof(b), "%lld", (long long)v);
    return dup_s(b);
}

const char *hl_float_to_str(double v) {
    /* the shortest form that reads back the same value, as Go prints floats */
    char b[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(b, sizeof(b), "%.*g", p, v);
        if (!isfinite(v) || strtod(b, NULL) == v) break;
    }
    return dup_s(b);
}

const char *hl_bool_to_str(int64_t v) { return v ? "true" : "false"; }

int64_t hl_str_to_int(const char *s) {
    char *end = NULL;
    errno = 0;
    long long v = strtoll(S(s), &end, 10);
    if (errno || end == S(s)) return 0;
    return v;
}

double hl_str_to_float(const char *s) {
    char *end = NULL;
    double v = strtod(S(s), &end);
    return end == S(s) ? 0.0 : v;
}

int64_t hl_contains(const char *s, const char *sub) { return strstr(S(s), S(sub)) != NULL; }

int64_t hl_has_prefix(const char *s, const char *p) {
    return strncmp(S(s), S(p), strlen(S(p))) == 0;
}

int64_t hl_has_suffix(const char *s, const char *p) {
    size_t ls = strlen(S(s)), lp = strlen(S(p));
    return lp <= ls && strcmp(S(s) + ls - lp, S(p)) == 0;
}

int64_t hl_index_of(const char *s, const char *sub) {
    const char *f = strstr(S(s), S(sub));
    return f ? (int64_t)(f - S(s)) : -1;
}

const char *hl_substr(const char *s, int64_t start, int64_t end) {
    int64_t n = hl_str_len(s);
    if (start < 0) start = 0;
    if (end > n) end = n;
    if (end <= start) return "";
    return dup_n(S(s) + start, (size_t)(end - start));
}

const char *hl_trim(const char *s) {
    const char *a = S(s);
    while (*a && isspace((unsigned char)*a)) a++;
    const char *e = a + strlen(a);
    while (e > a && isspace((unsigned char)e[-1])) e--;
    return dup_n(a, (size_t)(e - a));
}

const char *hl_upper(const char *s) {
    char *d = dup_s(s);
    for (char *p = d; *p; p++) *p = (char)toupper((unsigned char)*p);
    return d;
}

const char *hl_lower(const char *s) {
    char *d = dup_s(s);
    for (char *p = d; *p; p++) *p = (char)tolower((unsigned char)*p);
    return d;
}

const char *hl_replace(const char *s, const char *old, const char *rep) {
    const char *src = S(s), *o = S(old), *r = S(rep);
    size_t lo = strlen(o), lr = strlen(r);
    if (!lo) return src;
    size_t count = 0;
    for (const char *p = src; (p = strstr(p, o)); p += lo) count++;
    char *d = hl_alloc((int64_t)(strlen(src) + count * lr - count * lo + 1));
    char *w = d;
    for (const char *p = src;;) {
        const char *f = strstr(p, o);
        if (!f) { strcpy(w, p); break; }
        memcpy(w, p, (size_t)(f - p)); w += f - p;
        memcpy(w, r, lr); w += lr;
        p = f + lo;
    }
    return d;
}

static void arr_push_ptr(hl_arr **a, const void *v) {
    *a = hl_arr_grow(*a, 8);
    memcpy(hl_arr_at(*a, (*a)->len - 1, NULL, 0), &v, 8);
}

hl_arr *hl_split(const char *s, const char *sep) {
    hl_arr *a = hl_arr_new(8);
    const char *src = S(s), *sp = S(sep);
    size_t ls = strlen(sp);
    if (!ls) {
        for (const char *p = src; *p; p++) arr_push_ptr(&a, dup_n(p, 1));
        return a;
    }
    for (const char *p = src;;) {
        const char *f = strstr(p, sp);
        if (!f) { arr_push_ptr(&a, dup_s(p)); break; }
        arr_push_ptr(&a, dup_n(p, (size_t)(f - p)));
        p = f + ls;
    }
    return a;
}

const char *hl_join(const hl_arr *a, const char *sep) {
    int64_t n = hl_arr_len(a);
    size_t total = 1, ls = strlen(S(sep));
    for (int64_t i = 0; i < n; i++) {
        const char *e = *(const char **)(a->data + i * 8);
        total += strlen(S(e)) + ls;
    }
    char *d = hl_alloc((int64_t)total), *w = d;
    for (int64_t i = 0; i < n; i++) {
        const char *e = S(*(const char **)(a->data + i * 8));
        if (i) { memcpy(w, S(sep), ls); w += ls; }
        size_t le = strlen(e);
        memcpy(w, e, le); w += le;
    }
    *w = '\0';
    return d;
}

/* ---- arrays -------------------------------------------------------- */

hl_arr *hl_arr_new(int64_t esize) {
    hl_arr *a = hl_alloc(sizeof(*a));
    a->esize = esize;
    return a;
}

hl_arr *hl_arr_grow(hl_arr *a, int64_t esize) {
    if (!a) a = hl_arr_new(esize);
    if (a->len == a->cap) {
        int64_t cap = a->cap ? a->cap * 2 : 4;
        char *d = realloc(a->data, (size_t)(cap * a->esize));
        if (!d) hl_panic("out of memory");
        memset(d + a->cap * a->esize, 0, (size_t)((cap - a->cap) * a->esize));
        a->data = d;
        a->cap = cap;
    }
    a->len++;
    return a;
}

void *hl_arr_at(const hl_arr *a, int64_t i, const char *file, int64_t line) {
    int64_t n = hl_arr_len(a);
    if (i < 0 || i >= n) {
        char msg[96];
        snprintf(msg, sizeof(msg), "index %lld out of range [0:%lld]", (long long)i, (long long)n);
        hl_panic_at(msg, file ? file : "?", line);
    }
    return a->data + i * a->esize;
}

int64_t hl_arr_len(const hl_arr *a) { return a ? a->len : 0; }

/* ---- JSON values --------------------------------------------------- */

/* Bytes a value takes in an array element or a field: bool 1, the rest 8.
 * (A struct's `size` is the size of the memory it points to.) */
static int64_t slot_size(const hl_type *t) { return t->kind == HL_K_BOOL ? 1 : 8; }

static int64_t load_int(const void *slot) { int64_t v; memcpy(&v, slot, 8); return v; }
static double load_float(const void *slot) { double v; memcpy(&v, slot, 8); return v; }
static const void *load_ptr(const void *slot) { const void *v; memcpy(&v, slot, 8); return v; }

cJSON *hl_value_to_cjson(const hl_type *t, const void *slot) {
    switch (t->kind) {
    case HL_K_INT: return cJSON_CreateNumber((double)load_int(slot));
    case HL_K_FLOAT: return cJSON_CreateNumber(load_float(slot));
    case HL_K_BOOL: return cJSON_CreateBool(*(const unsigned char *)slot & 1);
    case HL_K_STRING: return cJSON_CreateString(S((const char *)load_ptr(slot)));
    case HL_K_ARRAY: {
        cJSON *arr = cJSON_CreateArray();
        const hl_arr *a = load_ptr(slot);
        for (int64_t i = 0; i < hl_arr_len(a); i++)
            cJSON_AddItemToArray(arr, hl_value_to_cjson(t->elem, a->data + i * a->esize));
        return arr;
    }
    case HL_K_STRUCT: {
        const char *base = load_ptr(slot);
        if (!base) return cJSON_CreateNull();
        cJSON *o = cJSON_CreateObject();
        for (int64_t i = 0; i < t->nfields; i++) {
            const hl_field *f = &t->fields[i];
            cJSON_AddItemToObject(o, f->name, hl_value_to_cjson(f->type, base + f->offset));
        }
        return o;
    }
    }
    return cJSON_CreateNull();
}

static void set_err(char **err, const char *path, const char *what) {
    if (!err || *err) return;
    char *m = NULL;
    if (asprintf(&m, "%s: %s", *path ? path : "/", what) < 0) m = NULL;
    *err = m;
}

int hl_value_from_cjson(const hl_type *t, const cJSON *j, void *slot, const char *path, char **err) {
    switch (t->kind) {
    case HL_K_INT: {
        if (!cJSON_IsNumber(j)) { set_err(err, path, "must be an integer"); return -1; }
        double d = j->valuedouble;
        if (d != floor(d)) { set_err(err, path, "must be an integer"); return -1; }
        int64_t v = (int64_t)d;
        memcpy(slot, &v, 8);
        return 0;
    }
    case HL_K_FLOAT: {
        if (!cJSON_IsNumber(j)) { set_err(err, path, "must be a number"); return -1; }
        double v = j->valuedouble;
        memcpy(slot, &v, 8);
        return 0;
    }
    case HL_K_BOOL:
        if (!cJSON_IsBool(j)) { set_err(err, path, "must be true or false"); return -1; }
        *(unsigned char *)slot = cJSON_IsTrue(j) ? 1 : 0;
        return 0;
    case HL_K_STRING: {
        if (!cJSON_IsString(j)) { set_err(err, path, "must be a string"); return -1; }
        const char *s = dup_s(j->valuestring);
        memcpy(slot, &s, 8);
        return 0;
    }
    case HL_K_ARRAY: {
        if (!cJSON_IsArray(j)) { set_err(err, path, "must be an array"); return -1; }
        hl_arr *a = hl_arr_new(slot_size(t->elem));
        int i = 0;
        for (const cJSON *e = j->child; e; e = e->next, i++) {
            a = hl_arr_grow(a, slot_size(t->elem));
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/%d", path, i);
            if (hl_value_from_cjson(t->elem, e, a->data + (a->len - 1) * a->esize, sub, err)) return -1;
        }
        memcpy(slot, &a, 8);
        return 0;
    }
    case HL_K_STRUCT: {
        if (!cJSON_IsObject(j)) { set_err(err, path, "must be an object"); return -1; }
        char *base = hl_alloc(t->size);
        for (int64_t i = 0; i < t->nfields; i++) {
            const hl_field *f = &t->fields[i];
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, f->name);
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/%s", path, f->name);
            if (!v) { set_err(err, sub, "is required"); return -1; }
            if (hl_value_from_cjson(f->type, v, base + f->offset, sub, err)) return -1;
        }
        memcpy(slot, &base, 8);
        return 0;
    }
    }
    set_err(err, path, "unsupported type");
    return -1;
}

cJSON *hl_type_schema(const hl_type *t, const char *desc) {
    cJSON *s = cJSON_CreateObject();
    switch (t->kind) {
    case HL_K_INT: cJSON_AddStringToObject(s, "type", "integer"); break;
    case HL_K_FLOAT: cJSON_AddStringToObject(s, "type", "number"); break;
    case HL_K_BOOL: cJSON_AddStringToObject(s, "type", "boolean"); break;
    case HL_K_STRING: cJSON_AddStringToObject(s, "type", "string"); break;
    case HL_K_ARRAY:
        cJSON_AddStringToObject(s, "type", "array");
        cJSON_AddItemToObject(s, "items", hl_type_schema(t->elem, NULL));
        break;
    case HL_K_STRUCT: {
        cJSON_AddStringToObject(s, "type", "object");
        cJSON *props = cJSON_AddObjectToObject(s, "properties");
        cJSON *req = cJSON_AddArrayToObject(s, "required");
        for (int64_t i = 0; i < t->nfields; i++) {
            cJSON_AddItemToObject(props, t->fields[i].name, hl_type_schema(t->fields[i].type, t->fields[i].desc));
            cJSON_AddItemToArray(req, cJSON_CreateString(t->fields[i].name));
        }
        cJSON_AddFalseToObject(s, "additionalProperties");
        break;
    }
    }
    if (desc && *desc) cJSON_AddStringToObject(s, "description", desc);
    return s;
}

const char *hl_to_json(const hl_type *t, const void *slot) {
    cJSON *j = hl_value_to_cjson(t, slot);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s ? s : "null";
}

const char *hl_value_text(const hl_type *t, const void *slot) {
    if (t->kind == HL_K_STRING) return S((const char *)load_ptr(slot));
    return hl_to_json(t, slot);
}

/* ---- printing ------------------------------------------------------ */

void hl_print_str(const char *s) { fputs(S(s), stdout); }
void hl_print_int(int64_t v) { printf("%lld", (long long)v); }
void hl_print_float(double v) { fputs(hl_float_to_str(v), stdout); }
void hl_print_bool(int64_t v) { fputs(v ? "true" : "false", stdout); }
void hl_print_value(const hl_type *t, const void *slot) { fputs(hl_value_text(t, slot), stdout); }
void hl_print_space(void) { fputc(' ', stdout); }
void hl_print_nl(void) { fputc('\n', stdout); }

/* ---- system -------------------------------------------------------- */

const char *hl_sh(const char *cmd) {
    fflush(stdout);
    FILE *p = popen(S(cmd), "r");
    if (!p) { g_exit_code = -1; return ""; }
    size_t cap = 4096, len = 0;
    char *buf = hl_alloc((int64_t)cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) hl_panic("out of memory");
            buf = nb;
        }
    }
    buf[len] = '\0';
    int st = pclose(p);
    g_exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return buf;
}

int64_t hl_exit_code(void) { return g_exit_code; }

const char *hl_read_file(const char *path) {
    FILE *f = fopen(S(path), "rb");
    if (!f) { g_exit_code = errno ? errno : 1; return ""; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = hl_alloc(n > 0 ? n + 1 : 1);
    size_t got = n > 0 ? fread(d, 1, (size_t)n, f) : 0;
    d[got] = '\0';
    fclose(f);
    g_exit_code = 0;
    return d;
}

int64_t hl_write_file(const char *path, const char *data) {
    FILE *f = fopen(S(path), "wb");
    if (!f) return 0;
    size_t n = strlen(S(data));
    int ok = fwrite(S(data), 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

int64_t hl_file_exists(const char *path) {
    struct stat st;
    return stat(S(path), &st) == 0;
}

const char *hl_getenv(const char *name) {
    const char *v = getenv(S(name));
    return v ? v : "";
}

hl_arr *hl_args(void) {
    hl_arr *a = hl_arr_new(8);
    for (int i = 1; i < g_argc; i++) arr_push_ptr(&a, g_argv[i]);
    return a;
}

void hl_exit(int64_t code) {
    fflush(stdout);
    exit((int)code);
}

void hl_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

int64_t hl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- entry --------------------------------------------------------- */

/* Defined by the agent runtime; absent in programs without agents. */
void hl_agents_describe(void) __attribute__((weak));

/* Decrypts the program's strings in place, once, before anything reads them. */
static void decrypt_strings(void) {
    uint8_t key[32], nonce[12];
    for (int i = 0; i < 32; i++) key[i] = hl_strkey[i] ^ hl_strmask[i];
    for (int64_t i = 0; i < hl_nstrs; i++) {
        hl_cc_nonce((uint64_t)i, nonce);
        hl_chacha20(key, nonce, (uint8_t *)hl_strtab[i].data, (size_t)hl_strtab[i].len);
    }
    memset(key, 0, sizeof(key));
}

int main(int argc, char **argv) {
    decrypt_strings();
    g_argc = argc;
    g_argv = argv;
    if (argc > 1 && strcmp(argv[1], "--hola-agents") == 0) {
        if (hl_agents_describe) hl_agents_describe();
        else puts("[]");
        return 0;
    }
    hl_fn_main();
    fflush(stdout);
    return 0;
}
