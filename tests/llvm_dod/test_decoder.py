#!/usr/bin/env python3
"""Compile the production descriptor slab against the real A64 pattern data."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[2]
header = (root/'src/common/cpu/recompiler/llvm/ir.h').read_text()
source = (root/'src/common/cpu/recompiler/llvm/ir_decoder.c').read_text()
lines=header.splitlines()
start=next(i for i,line in enumerate(lines) if line.startswith('#define SNX_IR_PATTERN_TYPES('))
end=start
while lines[end].rstrip().endswith('\\'):
    end+=1
mac='\n'.join(lines[start:end+1])
enum = re.search(r'typedef enum \{\s*SNX_IR_UNKNOWN,.*?\}\s*SnxIrPatternKind;', header, re.S)
if not enum:
    raise SystemExit('pattern enum missing')
functions = source[source.index('/* The guest decoder is immutable.') :]
boiler = '''#include "common/cpu/aarch64/a64.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
'''
boiler += mac+'\n'+enum.group(0)+'\n'
boiler += '''typedef struct { uintptr_t first,last; const SnxA64Pattern* pattern; } SnxIrCaseRange;
typedef struct {
    const SnxA64Pattern* patterns;
    SnxIrPatternKind* kinds;
    SnxIrCaseRange* ranges;
    size_t count;
    void* storage;
} SnxIrDecoder;
typedef struct { SnxIrDecoder decoder; } SnxIr;
'''
main = r'''
static void* decoder_worker(void* arg) {
    (void)arg;
    SnxIr ir = {0};
    if (ir_decoder_init(&ir)) return NULL;
    void* ptr = ir.decoder.storage;
    ir_decoder_free(&ir);
    return ptr;
}
int main(void) {
    pthread_t workers[16];
    for (size_t i = 0; i < 16; ++i)
        if (pthread_create(&workers[i], NULL, decoder_worker, NULL)) return 10;
    void* shared = NULL;
    for (size_t i = 0; i < 16; ++i) {
        void* value = NULL;
        if (pthread_join(workers[i], &value) || !value) return 11;
        if (shared && shared != value) return 12;
        shared = value;
    }
    SnxIr ir = {0};
    if (ir_decoder_init(&ir) || ir.decoder.storage != shared) return 1;
    size_t count = 0, checked = 0;
    const SnxA64Pattern* all = snx_a64_patterns(&count);
    if (ir.decoder.count != count || ir.decoder.patterns != all) return 2;
    if ((uintptr_t)ir.decoder.ranges % _Alignof(SnxIrCaseRange)) return 3;
    for (size_t i = 0; i < count; ++i) {
        const SnxA64Pattern* p = all + i;
        if (ir_pattern_kind(&ir, p) == SNX_IR_UNKNOWN) return 4;
        for (size_t j = 0; j < p->case_count; ++j) {
            size_t a = (size_t)-1, b = (size_t)-1;
            const SnxA64Case* c = &p->cases[j];
            const SnxA64Pattern* x = ir_case_pattern(&ir, c, &a);
            const SnxA64Pattern* y = snx_a64_case_pattern(c, &b);
            if (x != y || a != j || b != j) return 5;
            ++checked;
        }
    }
    unsigned seed = 0x13579bdf;
    for (size_t i = 0; i < 250000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const SnxA64Case* c = snx_a64_decode(seed);
        size_t a = 5, b = 8;
        const SnxA64Pattern* x = ir_case_pattern(&ir, c, &a);
        const SnxA64Pattern* y = snx_a64_case_pattern(c, &b);
        if (x != y || a != b) return 6;
    }
    SnxIr second = {0};
    if (ir_decoder_init(&second)) return 8;
    if (second.decoder.storage != ir.decoder.storage ||
        second.decoder.ranges != ir.decoder.ranges || second.decoder.kinds != ir.decoder.kinds)
        return 9;
    ir_decoder_free(&second);
    ir_decoder_free(&ir);
    if (ir.decoder.storage || ir.decoder.ranges || ir.decoder.count) return 7;
    printf("PASS decoder slab: %zu families, %zu cases, 250000 decoded instructions, shared slab, 16 concurrent initializers\n",count,checked);
    return 0;
}
'''
Path(sys.argv[1]).write_text(boiler+functions+main)
