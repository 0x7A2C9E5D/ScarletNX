#!/usr/bin/env python3
"""The ISA owns decoding. Verify concurrent publication and every case family."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[2]
Path(sys.argv[1]).write_text(r'''
#include "common/cpu/aarch64/a64.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void* worker(void* unused) {
    (void)unused;
    for (unsigned i = 0; i < 20000; ++i) {
        uint32_t word = i * 747796405u + 2891336453u;
        SnxA64Decoded d = snx_a64_decode_inst(word);
        if (d.instruction != snx_a64_decode(word)) return (void*)1;
        if (d.pattern && (d.pattern->kind == SNX_A64_UNKNOWN || d.index >= d.pattern->case_count ||
                          d.instruction != d.pattern->cases + d.index)) return (void*)2;
    }
    return NULL;
}
int main(void) {
    pthread_t threads[16];
    for (unsigned i = 0; i < 16; ++i) if (pthread_create(&threads[i], NULL, worker, NULL)) return 1;
    for (unsigned i = 0; i < 16; ++i) {
        void* result;
        if (pthread_join(threads[i], &result) || result) return 2;
    }
    size_t count = 0, cases = 0;
    const SnxA64Pattern* patterns = snx_a64_patterns(&count);
    for (size_t i = 0; i < count; ++i) {
        const SnxA64Pattern* p = &patterns[i];
        if (p->kind == SNX_A64_UNKNOWN) return 3;
        for (size_t j = 0; j < p->case_count; ++j) {
            size_t index = 123;
            if (snx_a64_case_pattern(p->cases + j, &index) != p || index != j) return 4;
            ++cases;
        }
    }
    if (count != 52 || cases != 1585) return 5;
    printf("PASS typed decoder: %zu families, %zu cases, 320000 concurrent decode checks\n", count, cases);
    return 0;
}
''')
