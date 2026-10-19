/* Frozen decoder fingerprint: compiles against both the old and new ISA API. */
#include "common/cpu/aarch64/a64.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t mix(uint64_t hash, uint64_t value) {
    for (unsigned k = 0; k != 8; ++k) {
        hash = (hash ^ (uint8_t)(value >> (k * 8))) * UINT64_C(1099511628211);
    }
    return hash;
}

int main(void) {
    size_t count, cases = 0;
    const SnxA64Pattern* patterns = snx_a64_patterns(&count);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count; ++i) {
        cases += patterns[i].case_count;
        hash = mix(hash, patterns[i].case_count);
    }
    uint32_t seed = 0x12345678;
    for (unsigned i = 0; i < 500000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t word = i < 250000 ? seed : ((i << 21) ^ seed);
        const SnxA64Case* instruction = snx_a64_decode(word);
        size_t index = 0;
        const SnxA64Pattern* pattern = snx_a64_case_pattern(instruction, &index);
#ifdef SNX_TYPED
        SnxA64Decoded decoded = snx_a64_decode_inst(word);
        if (decoded.instruction != instruction || decoded.pattern != pattern || decoded.index != index) return 3;
        if (pattern && (pattern->kind == SNX_A64_UNKNOWN || pattern->kind > SNX_A64_MSR_IMM)) return 4;
#endif
        hash = mix(hash, instruction->kind);
        hash = mix(hash, pattern ? (size_t)(pattern - patterns) + 1 : 0);
        hash = mix(hash, index);
    }
    printf("%zu families %zu cases %016llx\n", count, cases, (unsigned long long)hash);
    return count == 52 && cases == 1585 ? 0 : 1;
}
