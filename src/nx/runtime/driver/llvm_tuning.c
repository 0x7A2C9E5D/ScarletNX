/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* g_settings' LLVM Internals for the recompile (module_llvm.h's snx_llvm_tuning): opt's pipeline for the level with
   the passes turned off cut out of it, and the options set on the pass builder and the target machine */
#include "nx/loader/module_llvm.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "nx/runtime/settings_llvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a pipeline's text, cut */
typedef struct {
    const char* p; /* what is left to read */
    char* out;
    size_t at;
} Cut;

static void put(Cut* c, const char* s, size_t n) {
    memcpy(c->out + c->at, s, n);
    c->at += n;
}
/* the elements up to the end of their list (a ')' or the end), each a name, its <parameters> and its own pipeline in
   parentheses. A pass turned off is left out, and so is a pipeline left with none; require<> and invalidate<> stay.
   How many were kept */
static int cut_list(Cut* c) {
    int kept = 0;
    while (*c->p && *c->p != ')') {
        const char *start = c->p, *name_end;
        size_t mark = c->at;
        int depth = 0, keep = 1;
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        while (*c->p && !strchr(",()<", *c->p)) c->p++;
        name_end = c->p;
        if (*c->p == '<') /* its parameters, which may nest */
            for (; *c->p; c->p++) {
                if (*c->p == '<') depth++;
                if (*c->p == '>' && !--depth) {
                    c->p++;
                    break;
                }
            }
        if (kept) put(c, ",", 1);
        put(c, start, (size_t)(c->p - start));
        if (*c->p == '(') { /* a pipeline of its own */
            c->p++;
            put(c, "(", 1);
            keep = cut_list(c) > 0;
            if (*c->p == ')') c->p++;
            put(c, ")", 1);
        } else
            keep = settings_llvm_pass(&g_settings, start, (size_t)(name_end - start)) != 0;
        if (keep)
            kept++;
        else
            c->at = mark;
    }
    return kept;
}

static char g_pipeline[1 << 16];
static void builder(LLVMPassBuilderOptionsRef options) { settings_llvm_builder(&g_settings, options); }
static void machine(LLVMTargetMachineRef target) { settings_llvm_machine(&g_settings, target); }

const struct SnxIrTuning* snx_llvm_tuning(char opt, char* error, size_t error_size) {
    static SnxIrTuning tuning;
    const char* text = settings_llvm_pipeline(opt);
    tuning.pipeline = NULL;
    tuning.builder = builder;
    tuning.machine = machine;
    if (text) {
        Cut c;
        size_t n = strlen(text);
        if (n >= sizeof g_pipeline) {
            snprintf(error, error_size, "LLVM's pipeline for -O%c is longer than %u bytes", opt,
                     (unsigned)sizeof g_pipeline - 1);
            return NULL;
        }
        c.p = text;
        c.out = g_pipeline;
        c.at = 0;
        if (!cut_list(&c)) {
            snprintf(error, error_size, "LLVM Internals turn off every pass of -O%c's pipeline", opt);
            return NULL;
        }
        g_pipeline[c.at] = 0;
        /* opt's own text when nothing was cut: default<O?> runs the same, and doesn't depend on the text */
        if (strcmp(g_pipeline, text)) tuning.pipeline = g_pipeline;
    }
    return &tuning;
}
