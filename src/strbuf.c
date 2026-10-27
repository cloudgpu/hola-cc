/*
 * strbuf.c — growable string buffer for the compiler.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void) {
    fprintf(stderr, "holacc: out of memory\n");
    exit(70);
}

static void grow(strbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    char *d = realloc(b->data, cap);
    if (!d) die_oom();
    b->data = d;
    b->cap = cap;
}

void sb_putn(strbuf *b, const char *s, size_t n) {
    grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_putc(strbuf *b, char c) { sb_putn(b, &c, 1); }
void sb_puts(strbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

void sb_printf(strbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        grow(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

void sb_clear(strbuf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

void sb_free(strbuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

char *sb_take(strbuf *b) {
    char *s = b->data ? b->data : xstrdup("");
    b->data = NULL;
    b->len = b->cap = 0;
    return s;
}

char *xstrdup(const char *s) {
    char *d = strdup(s ? s : "");
    if (!d) die_oom();
    return d;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = NULL;
    if (vasprintf(&s, fmt, ap) < 0) die_oom();
    va_end(ap);
    return s;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) die_oom();
    return p;
}
