/*
 * strbuf.h — growable string buffer for the compiler.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#ifndef HOLACC_STRBUF_H
#define HOLACC_STRBUF_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t len, cap;
} strbuf;

void sb_putc(strbuf *b, char c);
void sb_puts(strbuf *b, const char *s);
void sb_putn(strbuf *b, const char *s, size_t n);
void sb_printf(strbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_clear(strbuf *b);
void sb_free(strbuf *b);
char *sb_take(strbuf *b);   /* returns the string (never NULL) and empties the buffer */

char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *xcalloc(size_t n, size_t size);

#endif
