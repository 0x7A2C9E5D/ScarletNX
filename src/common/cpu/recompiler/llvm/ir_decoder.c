/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/aarch64/cpu.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

/* The guest decoder is immutable. Share its lookup slab across LLVM workers. */
static const struct {
    const char* name;
    SnxIrPatternKind kind;
} pattern_names[] = {
#define SNX_IR_NAME(name) {#name, SNX_IR_##name},
    SNX_IR_PATTERN_TYPES(SNX_IR_NAME)
#undef SNX_IR_NAME
};

SnxIrPatternKind ir_pattern_kind(const SnxIr* ir, const SnxA64Pattern* pattern) {
    /* Decoder descriptors point into this immutable pattern array. */
    return pattern ? ir->decoder.kinds[pattern - ir->decoder.patterns] : SNX_IR_UNKNOWN;
}

/* The decoder's cases live in separate arrays. Sort their address ranges once;
   instruction translation then needs at most log2(pattern_count) comparisons. */
const SnxA64Pattern* ir_case_pattern(const SnxIr* ir, const SnxA64Case* instruction, size_t* index) {
    uintptr_t key = (uintptr_t)instruction;
    size_t lo = 0, hi = ir->decoder.count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ir->decoder.ranges[mid].first <= key)
            lo = mid + 1;
        else
            hi = mid;
    }
    *index = 0;
    if (!lo || key >= ir->decoder.ranges[lo - 1].last) return NULL;
    const SnxIrCaseRange* match = &ir->decoder.ranges[lo - 1];
    *index = (key - match->first) / sizeof(*instruction);
    return match->pattern;
}

/* A single immutable view is published with release/acquire ordering. Builders
   may race once at startup; only the winning slab lives for the process. No
   mutex, refcount or allocation on the translation path. */
static _Atomic(SnxIrDecoder*) shared_decoder;

static SnxIrDecoder* decoder_build(void) {
    size_t n;
    const SnxA64Pattern* patterns = snx_a64_patterns(&n);
    const size_t align = _Alignof(SnxIrCaseRange);
    if (n > (SIZE_MAX - sizeof(SnxIrDecoder) - align) / sizeof(SnxIrPatternKind)) return NULL;
    const size_t kinds_end = sizeof(SnxIrDecoder) + n * sizeof(SnxIrPatternKind);
    const size_t ranges_offset = (kinds_end + align - 1) & ~(align - 1);
    if (n > (SIZE_MAX - ranges_offset) / sizeof(SnxIrCaseRange)) return NULL;
    SnxIrDecoder* decoder = calloc(1, ranges_offset + n * sizeof(SnxIrCaseRange));
    if (!decoder) return NULL;
    decoder->patterns = patterns;
    decoder->count = n;
    decoder->storage = decoder; /* one process-lifetime allocation */
    decoder->kinds = (SnxIrPatternKind*)(decoder + 1);
    decoder->ranges = (SnxIrCaseRange*)((char*)decoder + ranges_offset);
    for (size_t i = 0; i < n; ++i) {
        const SnxA64Pattern* pattern = &decoder->patterns[i];
        const SnxIrCaseRange range = {(uintptr_t)pattern->cases, (uintptr_t)(pattern->cases + pattern->case_count),
                                      pattern};
        size_t j = i;
        while (j && decoder->ranges[j - 1].first > range.first) {
            decoder->ranges[j] = decoder->ranges[j - 1];
            --j;
        }
        decoder->ranges[j] = range;
        for (size_t k = 0; k < sizeof(pattern_names) / sizeof(*pattern_names); ++k) {
            if (strcmp(pattern->name, pattern_names[k].name)) continue;
            decoder->kinds[i] = pattern_names[k].kind;
            break;
        }
    }
    return decoder;
}

int ir_decoder_init(SnxIr* ir) {
    SnxIrDecoder* decoder = atomic_load_explicit(&shared_decoder, memory_order_acquire);
    if (!decoder) {
        SnxIrDecoder* fresh = decoder_build();
        if (!fresh) return -1;
        SnxIrDecoder* empty = NULL;
        if (atomic_compare_exchange_strong_explicit(&shared_decoder, &empty, fresh, memory_order_release,
                                                    memory_order_acquire)) {
            decoder = fresh;
        } else {
            decoder = empty;
            free(fresh);
        }
    }
    ir->decoder = *decoder; /* immutable arrays; the module owns nothing */
    return 0;
}

void ir_decoder_free(SnxIr* ir) { memset(&ir->decoder, 0, sizeof(ir->decoder)); }
