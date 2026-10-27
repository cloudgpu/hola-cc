/*
 * main.c — holacc, the compiler driver.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 *
 *   holacc build prog.hl [-o prog] [-O0..3] [--emit-llvm] [-v]
 *   holacc run prog.hl [-- args...]
 *   holacc check prog.hl
 *   holacc agents prog.hl          resolved agents as JSON
 *
 * Pipeline: flex/bison parser -> checker + LLVM IR (gen.c) -> clang links
 * the IR with the runtime into an x86_64 Linux ELF executable.
 */
#define _GNU_SOURCE
#include "ast.h"
#include "gen.h"
#include "strbuf.h"
#include "prelude.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HOLACC_RUNTIME_DIR
#define HOLACC_RUNTIME_DIR "."
#endif
#ifndef HOLA_ADK_DIR
#define HOLA_ADK_DIR ""
#endif
#ifndef HOLACC_VERSION
#define HOLACC_VERSION "dev"
#endif

static int g_verbose;
static int g_allow_plugins;   /* --allow-plugins: export symbols so HOLA_PLUGIN_DIR plugins can load */

static void usage(void) {
    fprintf(stderr,
            "usage: holacc <command> file.hl [options]\n"
            "  build file.hl [-o out] [-O0|-O1|-O2|-O3] [--emit-llvm] [--allow-plugins] [-v]\n"
            "                       compile to a native executable (ELF, x86_64 Linux)\n"
            "  run file.hl [-- args]  compile to a temporary file and run it\n"
            "  check file.hl        parse and type-check only\n"
            "  agents file.hl       print the resolved agents (after extends) as JSON\n"
            "  doctor               check that this machine can build agent programs\n"
            "  version              print the toolchain version\n"
            "environment: HOLACC_RUNTIME_DIR (runtime libraries), HOLA_ADK_DIR (Hola ADK),\n"
            "             CLANG (linker driver, default clang)\n");
}

/* ---- loading sources and imports ---------------------------------- */

static char **g_loaded;
static int g_nloaded;

static int load_file(const char *path, nodelist *out) {
    char real[PATH_MAX];
    if (!realpath(path, real)) { fprintf(stderr, "holacc: cannot open %s: %s\n", path, strerror(errno)); return -1; }
    for (int i = 0; i < g_nloaded; i++)
        if (strcmp(g_loaded[i], real) == 0) return 0;
    g_loaded = realloc(g_loaded, sizeof(*g_loaded) * (size_t)(g_nloaded + 1));
    g_loaded[g_nloaded++] = xstrdup(real);

    nodelist mine = {0};
    if (parse_file(path, &mine) != 0) return -1;
    int rc = 0;
    for (size_t i = 0; i < mine.len; i++) {
        node *n = mine.items[i];
        if (n->kind != N_IMPORT) { nodelist_push(out, n); continue; }
        char *dir = xstrdup(path);
        char *p = n->text[0] == '/' ? xstrdup(n->text) : xasprintf("%s/%s", dirname(dir), n->text);
        free(dir);
        if (load_file(p, out) != 0) rc = -1;
        free(p);
    }
    return rc;
}

static int load_program(const char *path, nodelist *out) {
    if (parse_string("<prelude>", k_prelude, out) != 0) {
        fprintf(stderr, "holacc: internal error: the prelude does not parse\n");
        return -1;
    }
    return load_file(path, out);
}

/* ---- running tools ------------------------------------------------- */

static int run_argv(char *const argv[]) {
    if (g_verbose) {
        for (int i = 0; argv[i]; i++) fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "holacc: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Where the runtime and the ADK are: the environment, else the toolchain
 * layout next to this executable (<prefix>/bin/holacc with
 * <prefix>/lib/holacc and <prefix>/lib/hola-adk), else the build tree. */
static char *beside_exe(const char *rel, const char *probe) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return NULL;
    exe[n] = '\0';
    char *dir = dirname(exe);
    char *cand = xasprintf("%s/../%s", dir, rel);
    char *p = xasprintf("%s/%s", cand, probe);
    int ok = access(p, R_OK) == 0;
    free(p);
    if (!ok) { free(cand); return NULL; }
    char real[PATH_MAX];
    char *r = realpath(cand, real) ? xstrdup(real) : cand;
    if (r != cand) free(cand);
    return r;
}

static const char *runtime_dir(void) {
    static char *d;
    if (getenv("HOLACC_RUNTIME_DIR") && *getenv("HOLACC_RUNTIME_DIR")) return getenv("HOLACC_RUNTIME_DIR");
    if (!d) d = beside_exe("lib/holacc", "libholart.a");
    return d ? d : HOLACC_RUNTIME_DIR;
}

static const char *adk_dir(void) {
    static char *d;
    if (getenv("HOLA_ADK_DIR") && *getenv("HOLA_ADK_DIR")) return getenv("HOLA_ADK_DIR");
    if (!d) d = beside_exe("lib/hola-adk", "lib/libhola.a");
    return d ? d : HOLA_ADK_DIR;
}

static const char *env_or(const char *name, const char *def) {
    const char *v = getenv(name);
    return v && *v ? v : def;
}

static int link_program(const char *ll, const char *out, const char *opt, int uses_agents) {
    const char *rt = runtime_dir();
    const char *adk = adk_dir();
    char *argv[40];
    int n = 0;
    argv[n++] = (char *)env_or("CLANG", "clang");
    argv[n++] = (char *)opt;
    argv[n++] = "-Wno-override-module";
    argv[n++] = (char *)ll;
    argv[n++] = "-o";
    argv[n++] = (char *)out;
    if (uses_agents) {
        argv[n++] = xasprintf("%s/libholart_agent.a", rt);
        argv[n++] = "-Wl,-u,hl_agents_describe";
    }
    argv[n++] = xasprintf("%s/libholart.a", rt);
    if (uses_agents) {
        char *lib = xasprintf("%s/lib/libhola.a", adk);
        if (!*adk || access(lib, R_OK) != 0) {
            fprintf(stderr, "holacc: this program declares agents and needs the Hola ADK; set HOLA_ADK_DIR "
                            "(build one with scripts/build-adk.sh in the Hola repo)\n");
            return 1;
        }
        argv[n++] = xasprintf("%s/lib/libhola_tools.a", adk);
        argv[n++] = "-Wl,--whole-archive";
        argv[n++] = lib;
        argv[n++] = "-Wl,--no-whole-archive";
    }
    argv[n++] = xasprintf("%s/libcjson.a", rt);
    if (uses_agents) {
        argv[n++] = "-lcurl";
        argv[n++] = "-lsqlite3";
        argv[n++] = "-lpthread";
        argv[n++] = "-ldl";
        if (g_allow_plugins) argv[n++] = "-rdynamic";
    }
    argv[n++] = "-lm";
    if (!getenv("HOLACC_DEBUG")) argv[n++] = "-s";   /* no symbol or debug names in what ships */
    argv[n] = NULL;
    return run_argv(argv);
}

static int compile(const char *src, const char *out, const char *opt, int emit_llvm) {
    nodelist prog = {0};
    if (load_program(src, &prog) != 0) return 1;
    char *ll;
    if (emit_llvm) {
        ll = xstrdup(out);
    } else {
        ll = xasprintf("%s.ll", out);
    }
    gen_result r = gen_program(&prog, ll);
    if (r.errors) {
        fprintf(stderr, "%d error%s\n", r.errors, r.errors == 1 ? "" : "s");
        return 1;
    }
    if (emit_llvm) return 0;
    int rc = link_program(ll, out, opt, r.uses_agents);
    if (!getenv("HOLACC_KEEP_LL")) unlink(ll);
    return rc;
}

/* holacc doctor: can this machine build and link agent programs? */
static int doctor(void) {
    int bad = 0;
    const char *rt = runtime_dir(), *adk = adk_dir();
    char *p;
    printf("holacc %s\n", HOLACC_VERSION);
    p = xasprintf("%s/libholart.a", rt);
    printf("  runtime   %-50s %s\n", rt, access(p, R_OK) == 0 ? "ok" : (bad = 1, "MISSING"));
    p = xasprintf("%s/lib/libhola.a", adk);
    int have_adk = *adk && access(p, R_OK) == 0;
    printf("  Hola ADK  %-50s %s\n", *adk ? adk : "(not set)", have_adk ? "ok" : "missing: programs with agents cannot link");
    if (have_adk) {
        char *vf = xasprintf("%s/VERSION", adk);
        FILE *f = fopen(vf, "r");
        char v[64] = "";
        if (f) { if (!fgets(v, sizeof(v), f)) v[0] = '\0'; fclose(f); v[strcspn(v, "\n")] = '\0'; }
        if (*v) printf("  ADK version %s\n", v);
    }
    /* link a tiny program the way agent programs are linked */
    char dir[] = "/tmp/holacc-doctor-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char *ll = xasprintf("%s/t.ll", dir), *out = xasprintf("%s/t", dir);
    FILE *f = fopen(ll, "w");
    if (f) { fputs("define i32 @main() {\n  ret i32 0\n}\n", f); fclose(f); }
    const char *clang = env_or("CLANG", "clang");
    char *argv1[] = {(char *)clang, "-Wno-override-module", ll, "-o", out, NULL};
    int saved = g_verbose;
    int fd_null = -1;
    (void)fd_null;
    g_verbose = 0;
    int rc1 = run_argv(argv1);
    printf("  clang     %-50s %s\n", clang, rc1 == 0 ? "ok" : (bad = 1, "cannot compile LLVM IR (install clang)"));
    char *argv2[] = {(char *)clang, "-Wno-override-module", ll, "-o", out, "-lcurl", "-lsqlite3", "-lpthread", "-ldl", "-lm", NULL};
    int rc2 = rc1 == 0 ? run_argv(argv2) : 1;
    printf("  libraries %-50s %s\n", "libcurl, libsqlite3", rc2 == 0 ? "ok" :
           "cannot link (install the libcurl and sqlite3 development packages)");
    if (rc2 != 0) bad = 1;
    g_verbose = saved;
    unlink(ll); unlink(out); rmdir(dir);
    printf(bad ? "not ready\n" : "ready\n");
    return bad;
}

static char *default_out(const char *src) {
    char *b = xstrdup(src);
    char *base = basename(b);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *o = xstrdup(base);
    free(b);
    return o;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("holacc %s (Hola Agent Toolchain)\n", HOLACC_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "doctor") == 0) return doctor();
    if (argc < 3) { usage(); return 2; }
    const char *cmd = argv[1];
    const char *src = argv[2];
    const char *out = NULL;
    const char *opt = "-O2";
    int emit_llvm = 0;
    int rest = argc;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { rest = i + 1; break; }
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (strncmp(argv[i], "-O", 2) == 0) opt = argv[i];
        else if (strcmp(argv[i], "--emit-llvm") == 0) emit_llvm = 1;
        else if (strcmp(argv[i], "-v") == 0) g_verbose = 1;
        else if (strcmp(argv[i], "--allow-plugins") == 0) g_allow_plugins = 1;
        else { fprintf(stderr, "holacc: unknown option %s\n", argv[i]); usage(); return 2; }
    }

    if (strcmp(cmd, "check") == 0) {
        nodelist prog = {0};
        if (load_program(src, &prog) != 0) return 1;
        gen_result r = gen_program(&prog, NULL);
        if (r.errors) { fprintf(stderr, "%d error%s\n", r.errors, r.errors == 1 ? "" : "s"); return 1; }
        return 0;
    }
    if (strcmp(cmd, "agents") == 0) {
        nodelist prog = {0};
        if (load_program(src, &prog) != 0) return 1;
        return gen_describe_agents(&prog);
    }
    if (strcmp(cmd, "build") == 0) {
        if (!out) out = emit_llvm ? xasprintf("%s.ll", default_out(src)) : default_out(src);
        return compile(src, out, opt, emit_llvm);
    }
    if (strcmp(cmd, "run") == 0) {
        char dir[] = "/tmp/holacc-XXXXXX";
        const char *tmpdir = getenv("HOLACC_TMPDIR");
        char *tmpl = tmpdir ? xasprintf("%s/holacc-XXXXXX", tmpdir) : xstrdup(dir);
        if (!mkdtemp(tmpl)) { perror("holacc: mkdtemp"); return 1; }
        char *bin = xasprintf("%s/%s", tmpl, default_out(src));
        int rc = compile(src, bin, opt, 0);
        if (rc) return rc;
        char **av = xcalloc((size_t)(argc - rest + 2), sizeof(char *));
        av[0] = bin;
        for (int i = rest; i < argc; i++) av[1 + i - rest] = argv[i];
        rc = run_argv(av);
        unlink(bin);
        rmdir(tmpl);
        return rc;
    }
    usage();
    return 2;
}
