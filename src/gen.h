/*
 * gen.h — type checking, agent resolution and LLVM IR generation.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
#ifndef HOLACC_GEN_H
#define HOLACC_GEN_H

#include "ast.h"

typedef struct {
    int uses_agents;   /* the program declares at least one agent */
    int errors;
} gen_result;

/* Checks the program and writes LLVM IR to `ir_path` (NULL: check only). */
gen_result gen_program(nodelist *top, const char *ir_path);

/* Prints the resolved agents as JSON manifests (holacc agents). */
int gen_describe_agents(nodelist *top);

#endif
