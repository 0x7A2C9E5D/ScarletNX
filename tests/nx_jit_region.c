/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's coverage of code outside every image (guest_jit.c's jit_resolve_or_add): a pc in a block the game
   mapped executable is compiled through an image of its own, and the image goes when the block is unmapped or stops
   being executable. The memory is this file's, so no loader runs: mem_query answers from the block below */
#include "nx/runtime/guest_jit.c" /* the code under test; the runtime it talks to is stubbed below */
#include <stdio.h>
#include <string.h>

#define LIVE       0x8000 /* the block the game maps, outside the dispatch range */
#define BLOCK_SIZE 0x1000

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---- the runtime as much of it as guest_jit.c uses ---- */
static u8 memory[0x20000];
u8* g_mem = memory;
static SnxModule module; /* no modules: nothing the service can compile at first */
const SnxModule* g_mod = &module;
GuestFn* g_disp;
u64 g_code_lo, g_code_hi;
int g_opt_jit_threads = 1;
int g_verbose;
char g_cachedir[1024], g_exename[256] = "nx_jit_region_test";

static struct {
    u64 addr, size, state;
    u32 perm;
} block = {LIVE, BLOCK_SIZE, MS_CODE, PERM_RX};

void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    if (addr >= block.addr && addr - block.addr < block.size) {
        out->addr = block.addr;
        out->size = block.size;
        out->state = block.state;
        out->perm = block.perm;
    } else {
        out->addr = addr & ~(u64)0xFFF;
        out->size = 0x1000;
        out->state = MS_FREE;
    }
}

static void (*invalidate_hook)(u64, u64);
void mem_set_code_invalidate(void (*fn)(u64, u64)) { invalidate_hook = fn; }
u64* hints_copy(size_t* count) {
    *count = 0;
    return NULL;
}
int nvn_trace_active(void) { return 0; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
/* single-threaded: the synthetic images' lock has nothing to do here */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }

static void put(u64 address, u32 word) { memcpy(memory + address, &word, sizeof word); }
static size_t synthetic_images(void) {
    size_t i, n = 0;
    for (i = 0; i < SNX_JIT_IMAGES; ++i)
        if (g_jit_synthetic[i].size) ++n;
    return n;
}

int main(void) {
    GuestFn f;
    g_code_lo = 0x10000;
    g_code_hi = 0x20000;
    g_disp = (GuestFn*)calloc((g_code_hi - g_code_lo) / 4, sizeof *g_disp);
    if (!g_disp) return 1;
    /* add x0, x0, #5; ret */
    put(LIVE, 0x91001400);
    put(LIVE + 4, 0xD65F03C0);
    jit_init();
    CHECK(g_jit && invalidate_hook != NULL); /* the JIT started and registered the invalidation hook */
    CHECK(jit_resolve_or_add(LIVE) == NULL); /* the first miss only queues the compile */
    f = jit_wait(LIVE);
    CHECK(f != NULL); /* compiled from an image of the block */
    snx_jit_drain(g_jit);
    CHECK(snx_jit_resolve(g_jit, LIVE) == f); /* the image's own table finds it without a wait */
    CHECK(synthetic_images() == 1);
    /* the block goes: mem.c calls the hook after the mapping change */
    block.state = MS_FREE;
    block.perm = PERM_NONE;
    invalidate_hook(LIVE, BLOCK_SIZE);
    CHECK(synthetic_images() == 0); /* its image went with it */
    CHECK(snx_jit_resolve(g_jit, LIVE) == NULL);
    CHECK(jit_resolve_or_add(LIVE) == NULL); /* nothing to compile in freed memory */
    /* mapped again, executable, with other code: a new image and new code */
    put(LIVE, 0x91002800); /* add x0, x0, #10 */
    block.state = MS_CODE;
    block.perm = PERM_RX;
    CHECK(jit_resolve_or_add(LIVE) == NULL); /* a new image, and the compile queued */
    CHECK(jit_wait(LIVE) != NULL);
    CHECK(synthetic_images() == 1);
    /* the block becomes data: readable, not executable */
    block.state = MS_NORMAL;
    block.perm = PERM_RW;
    invalidate_hook(LIVE, BLOCK_SIZE);
    CHECK(synthetic_images() == 0);
    CHECK(jit_resolve_or_add(LIVE) == NULL);
    snx_jit_service_free(g_jit);
    g_jit = NULL;
    remove("nx_jit_region_test.jitcache");
    if (failures) {
        fprintf(stderr, "nx jit region: %d failures\n", failures);
        return 1;
    }
    printf("nx jit region: passed\n");
    return 0;
}
