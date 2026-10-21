/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's side of the AOT pack: the variant table is looked up by the current
   guest bytes, a hit fills the dispatch slot, a changed word is refused, and
   invalidation clears the slot. No compiler is involved. */
#include "common/cpu/recompiler/variants.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_LO 0x1000
#define CODE_HI 0x3000
#define START   0x2000
#define OUT     0x3000
#define OUT_SIZE 0x800

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static u8 g_memory[0x4000];
u8* g_mem = g_memory;

static MemBlock g_block;
static SnxModInfo g_mods[1];
static SnxModule g_module;
const SnxModule* g_mod = &g_module;

static GuestFn g_dispatch[(CODE_HI - CODE_LO) / 4];
GuestFn* g_disp = g_dispatch;
u64 g_code_lo = CODE_LO, g_code_hi = CODE_HI;

static SnxVariantRun g_run, g_run2;
static SnxU64 g_leader, g_leader2;
static SnxVariantFunction g_function, g_function2;
static SnxVariantFunction g_functions[2];
static SnxVariants g_variants_table;
const SnxVariants* g_variants = &g_variants_table;

static int g_called;
static void test_fn(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
    ++g_called;
}

static int g_called2;
static void test_fn2(CPU* c, u64 entry) {
    (void)c;
    (void)entry;
    ++g_called2;
}

static int word(void* user, SnxU64 address, SnxU32* out) {
    (void)user;
    if (address > sizeof(g_memory) - 4 || (address & 3)) return -1;
    memcpy(out, g_memory + address, sizeof(*out));
    return 0;
}

void mem_query(u64 addr, MemBlock* out) {
    (void)addr;
    *out = g_block;
}

static void (*g_invalidate)(u64, u64);
void mem_set_code_invalidate(void (*fn)(u64, u64)) { g_invalidate = fn; }

void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}

int main(void) {
    static const SnxU32 words[] = {0x91000400u, 0xD65F03C0u};  /* add x0, x0, #1; ret */
    static const SnxU32 words2[] = {0x91000800u, 0xD65F03C0u}; /* add x0, x0, #2; ret */
    GuestFn fn;
    memcpy(g_memory + START, words, sizeof(words));
    memcpy(g_memory + OUT, words2, sizeof(words2));
    memset(&g_block, 0, sizeof(g_block));
    g_block.addr = CODE_LO;
    g_block.size = OUT + OUT_SIZE - CODE_LO;
    g_block.state = MS_CODE;
    g_block.perm = PERM_RX;
    memset(g_mods, 0, sizeof(g_mods));
    g_mods[0].name = "test";
    g_mods[0].base = CODE_LO;
    g_mods[0].size = CODE_HI - CODE_LO;
    g_mods[0].text_off = 0;
    g_mods[0].text_size = CODE_HI - CODE_LO;
    memset(&g_module, 0, sizeof(g_module));
    g_module.abi = SNX_ABI;
    g_module.nmods = 1;
    g_module.mods = g_mods;
    g_module.code_lo = CODE_LO;
    g_module.code_hi = CODE_HI;
    g_module.title = "test";
    g_leader = START;
    g_run.address = START;
    g_run.words = 2;
    g_function.start = START;
    g_function.leader_count = 1;
    g_function.leaders = &g_leader;
    g_function.run_count = 1;
    g_function.runs = &g_run;
    g_function.fn = test_fn;
    CHECK(snx_variant_hash(word, NULL, &g_run, 1, &g_function.h1, &g_function.h2) == 0);
    g_leader2 = OUT;
    g_run2.address = OUT;
    g_run2.words = 2;
    g_function2.start = OUT;
    g_function2.leader_count = 1;
    g_function2.leaders = &g_leader2;
    g_function2.run_count = 1;
    g_function2.runs = &g_run2;
    g_function2.fn = test_fn2;
    CHECK(snx_variant_hash(word, NULL, &g_run2, 1, &g_function2.h1, &g_function2.h2) == 0);
    g_functions[0] = g_function;
    g_functions[1] = g_function2;
    g_variants_table.version = SNX_VARIANTS_VERSION;
    g_variants_table.count = 2;
    g_variants_table.functions = g_functions;

    jit_init();
    CHECK(g_invalidate == jit_code_invalidate);
    CHECK(g_disp[(START - CODE_LO) / 4] == NULL);
    fn = jit_wait(START);
    CHECK(fn == test_fn);
    if (fn) fn(NULL, START);
    CHECK(g_called == 1);
    CHECK(g_disp[(START - CODE_LO) / 4] == test_fn);
    CHECK(jit_resolve(START) == test_fn);
    CHECK(jit_wait(START) == test_fn);

    g_memory[START] ^= 1;
    jit_code_invalidate(START, 4);
    CHECK(g_disp[(START - CODE_LO) / 4] == NULL);
    CHECK(jit_wait(START) == NULL);
    CHECK(jit_resolve(START) == NULL);

    g_memory[START] ^= 1;
    CHECK(jit_wait(START) == test_fn);
    /* a pc outside code_lo..hi is looked up through a block the game mapped */
    CHECK(jit_wait(OUT) == NULL);
    fn = jit_resolve_or_add(OUT);
    CHECK(fn == test_fn2);
    if (fn) fn(NULL, OUT);
    CHECK(g_called2 == 1);
    CHECK(jit_resolve(OUT) == test_fn2);
    /* the block stops being code: invalidation drops the synthetic image */
    g_block.state = MS_NORMAL;
    g_block.perm = PERM_RW;
    jit_code_invalidate(OUT, 4);
    CHECK(jit_wait(OUT) == NULL);
    CHECK(jit_resolve_or_add(OUT) == NULL);
    /* a pc outside the table, in the same block, has no variant */
    CHECK(jit_wait(OUT + 0x400) == NULL);
    CHECK(jit_resolve(OUT + 0x400) == NULL);
    CHECK(jit_resolve_or_add(OUT + 0x400) == NULL);
    jit_report();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("runtime_variants: ok");
    return 0;
}
