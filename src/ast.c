/*
 * ast.c — syntax tree allocation.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#include "ast.h"
#include "strbuf.h"

#include <stdlib.h>

node *node_new(nkind k, int line) {
    node *n = xcalloc(1, sizeof(*n));
    n->kind = k;
    n->line = line;
    n->file = g_cur_file;
    return n;
}

void nodelist_push(nodelist *l, node *n) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, l->cap * sizeof(*l->items));
        if (!l->items) abort();
    }
    l->items[l->len++] = n;
}
