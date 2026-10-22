/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The blocks the game module lacks, compiled while the game runs by the ARM backend's executor (cpu/arm/exec.h): an
   indirect branch's target discovery didn't find, the runtime's trampolines. Each is recorded in the game's hints, so
   the next compile has it */
#include "vita/runtime/jit.h"
#include "common/utilities/common.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef SNX_VITA_JIT /* a build without the executor (snx_llvm comes with the Switch runtime) */
VitaJit* vita_jit_new(VitaProcess* p, char* error, size_t error_size) {
    (void)p;
    snprintf(error, error_size, "this build has none");
    return NULL;
}
SnxArmBlock vita_jit_block(VitaJit* jit, uint32_t pc, bool thumb) {
    (void)jit, (void)pc, (void)thumb;
    return NULL;
}
size_t vita_jit_save_hints(VitaJit* jit, const char* folder) {
    (void)jit, (void)folder;
    return 0;
}
void vita_jit_free(VitaJit* jit) { (void)jit; }
#else
#include "common/cpu/arm/exec.h"

struct VitaJit {
    VitaProcess* process;
    SnxArmExec* exec;
    SnxArmExecRange ranges[VITA_SEGMENTS + 1];
    size_t range_count;
    uint32_t* compiled; /* the image's blocks compiled here, for the hints */
    size_t count, capacity;
};
VitaJit* vita_jit_new(VitaProcess* p, char* error, size_t error_size) {
    VitaJit* j = calloc(1, sizeof *j);
    if (!j) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    j->process = p;
    if (!(j->exec = snx_arm_exec_new(error, error_size))) {
        free(j);
        return NULL;
    }
    for (unsigned i = 0; i < p->image.segment_count; ++i)
        if (p->image.segment[i].flags & 1)
            j->ranges[j->range_count++] = (SnxArmExecRange){p->image.segment[i].address, NULL, p->image.segment[i].size};
    j->ranges[j->range_count++] = (SnxArmExecRange){VITA_RUNTIME_PAGE, NULL, 0x1000};
    return j;
}
SnxArmBlock vita_jit_block(VitaJit* j, uint32_t pc, bool thumb) {
    SnxArmBlock block = snx_arm_exec_block(j->exec, j->ranges, j->range_count, pc, thumb, j->process->mem);
    if (!block) return NULL;
    if (pc < VITA_RUNTIME_PAGE || pc >= VITA_RUNTIME_PAGE + 0x1000) {
        bool known = false;
        for (size_t i = 0; i < j->count && !known; ++i) known = j->compiled[i] == (pc | thumb);
        if (!known) {
            if (j->count == j->capacity) {
                size_t capacity = j->capacity ? j->capacity * 2 : 256;
                uint32_t* grown = realloc(j->compiled, capacity * sizeof *grown);
                if (grown) j->compiled = grown, j->capacity = capacity;
            }
            if (j->count < j->capacity) j->compiled[j->count++] = pc | thumb;
            if (g_settings.vita_run_time_compile_trace) fprintf(stderr, "vita jit: compiled %08x\n", (unsigned)(pc | thumb));
        }
    }
    return block;
}
size_t vita_jit_save_hints(VitaJit* j, const char* folder) {
    char* path = j && j->count ? vita_game_file(folder, VITA_HINTS_SUFFIX) : NULL;
    FILE* f = path ? fopen(path, "a") : NULL;
    size_t added = 0;
    for (size_t i = 0; f && i < j->count; ++i)
        if (fprintf(f, "%08x\n", (unsigned)j->compiled[i]) > 0) ++added;
    if (f && fclose(f)) added = 0;
    free(path);
    return added;
}
void vita_jit_free(VitaJit* j) {
    if (!j) return;
    snx_arm_exec_free(j->exec);
    free(j->compiled);
    free(j);
}
#endif
