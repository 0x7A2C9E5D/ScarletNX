/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's NX side (jit.h): the game's code ranges and the module's entries handed to the service, and the
   hints of earlier runs queued for it. There is no interpreter behind it: a JIT that can't start leaves only the code
   the recompiler found, and the first pc it missed ends the game with the reason (rtapi.c's rt_miss) */
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/rtapi.h"
#include "nx/gpu/nvn_trace.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/cpu/recompiler/llvm/jit_service.h"
#include <stdlib.h>
#include <string.h>

static SnxJitService* g_jit;
static char g_jit_error[512];      /* why there is no JIT, for the fatal error of the first miss */
static char g_jit_cache[1400];     /* <Game>.jitcache in the user's cache folder: the units of earlier runs */
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for as long as the service lives */

/* the text one image covers from the pc that missed: a block the game maps may be large, and an image's table is one
   GuestFn for every 4 bytes of its text */
#define JIT_SYNTHETIC_MAX (4u << 20)
/* A block of code outside every image the loader gave the service, which the game mapped itself: the pc execution
   reached, and the text from it the image covers. While the block is executable code the image stays, and its next
   compile reads the bytes as they are then; jit_code_invalidate drops the image when it stops being code */
typedef struct {
    u64 base, size;
} Synthetic;
static Synthetic g_jit_synthetic[SNX_JIT_IMAGES];
static PMutex g_jit_synthetic_lock = PMUTEX_INIT;

/* on a compile thread */
static void jit_log(const SnxJitReport* r) {
    if (r->dropped)
        LOGV("jit: %llx and %lu more pcs queued again: guest memory changed while they compiled\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1));
    else if (r->error)
        WARN_LIMIT(32, "jit: %llx and %lu more pcs could not be compiled: %s\n", (unsigned long long)r->pc,
                   (unsigned long)(r->pcs - 1), r->error);
    else
        LOGV("jit: %llx and %lu more pcs compiled, %lu functions, %lu instructions, %lu KiB, %.1f ms\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1), (unsigned long)r->functions,
             (unsigned long)r->instructions, (unsigned long)(r->bytes >> 10), r->seconds * 1000.0);
}

void jit_init(void) {
    SnxJitConfig config;
    SnxJitStats stats;
    char error[512];
    u64* hints;
    size_t hint_count, warm = 0;
    u32 i;
    g_jit_images = (SnxCodeImage*)calloc(g_mod->nmods ? g_mod->nmods : 1, sizeof(*g_jit_images));
    if (!g_jit_images) {
        snprintf(g_jit_error, sizeof(g_jit_error), "out of memory");
        WARN("jit: out of memory, so only the code the recompiler found can run\n");
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        g_jit_images[i].image = GP(m->base);
        g_jit_images[i].image_size = (size_t)m->size;
        g_jit_images[i].base = m->base;
        g_jit_images[i].text_off = m->text_off;
        g_jit_images[i].text_size = m->text_size;
    }
    memset(&config, 0, sizeof(config));
    config.images = g_jit_images;
    config.image_count = g_mod->nmods;
    config.dispatch = g_disp;
    config.code_lo = g_code_lo;
    config.code_hi = g_code_hi;
    config.aot_addresses = g_mod->entry_addr;
    config.aot_functions = g_mod->entry_fn;
    config.aot_count = g_mod->nentries;
    /* with SNX_NVN_TRACE, wrappers sit in dispatch slots, which only calls through the table see */
    config.hook_direct_calls = nvn_trace_active();
    config.direct_svc = 1; /* g_rtapi's svcs, as the AOT module calls them */
    config.threads = (unsigned)g_settings.jit_threads;
    config.report = jit_log;
    snprintf(g_jit_cache, sizeof(g_jit_cache), "%s%s.jitcache", g_cachedir, g_exename);
    config.cache_path = g_jit_cache;
    g_jit = snx_jit_service_new(&config, error, sizeof(error));
    if (!g_jit) {
        snprintf(g_jit_error, sizeof(g_jit_error), "%s", error);
        WARN("jit: cannot start (%s), so only the code the recompiler found can run\n", error);
        free(g_jit_images);
        g_jit_images = NULL;
        return;
    }
    /* warm start: what earlier runs missed is compiled before the game reaches it, if the JIT has time before then */
    hints = hints_copy(&hint_count);
    if (hints) warm = snx_jit_warm(g_jit, hints, hint_count);
    free(hints);
    snx_jit_stats(g_jit, &stats);
    LOG("jit: guest code the recompiler missed is compiled when a thread reaches it, on %u threads. %lu pcs of earlier "
        "runs queued\n",
        stats.threads, (unsigned long)warm);
    /* mapping, permission and write-back changes drop what was compiled from the range (mem.c) */
    mem_set_code_invalidate(jit_code_invalidate);
}

GuestFn jit_resolve(u64 pc) { return g_jit ? snx_jit_resolve(g_jit, pc) : NULL; }

/* whether pc is in executable guest memory, and the block it is in */
static int code_at(u64 pc, MemBlock* block) {
    if (pc & 3) return 0;
    mem_query(pc, block);
    return block->state != MS_FREE && (block->perm & PERM_X) && block->size >= 4 && pc >= block->addr &&
           pc - block->addr < block->size;
}

/* the image for the block pc is in, unless the service has one of the block already; 1 when resolve can compile pc */
static int jit_add_synthetic(u64 pc) {
    MemBlock block;
    SnxCodeImage image;
    u64 text;
    size_t i, slot = SNX_JIT_IMAGES;
    if (!code_at(pc, &block)) return 0;
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        if (g_jit_synthetic[i].size && pc - g_jit_synthetic[i].base < g_jit_synthetic[i].size) {
            pm_unlock(&g_jit_synthetic_lock); /* inside one already: its compile is queued or running */
            return 1;
        }
        if (!g_jit_synthetic[i].size && slot == SNX_JIT_IMAGES) slot = i;
    }
    if (slot == SNX_JIT_IMAGES) {
        pm_unlock(&g_jit_synthetic_lock);
        return 0; /* every image slot is a block the game mapped: this pc can't be compiled */
    }
    text = block.addr + block.size - pc;
    if (text > JIT_SYNTHETIC_MAX) text = JIT_SYNTHETIC_MAX;
    memset(&image, 0, sizeof(image));
    image.image = GP(pc);
    image.image_size = (size_t)text;
    image.base = pc;
    image.text_size = text;
    if (snx_jit_add_image(g_jit, &image)) {
        pm_unlock(&g_jit_synthetic_lock); /* overlapping an image added meanwhile, or out of memory */
        return 0;
    }
    g_jit_synthetic[slot].base = pc;
    g_jit_synthetic[slot].size = text;
    pm_unlock(&g_jit_synthetic_lock);
    return 1;
}

GuestFn jit_resolve_or_add(u64 pc) {
    GuestFn code;
    if (!g_jit) return NULL;
    code = snx_jit_resolve(g_jit, pc);
    if (code) return code;
    /* an image the service has holds it (queued or compiling), or the block it is in is code the game mapped */
    if (snx_jit_covers(g_jit, pc) || !jit_add_synthetic(pc)) return NULL;
    return snx_jit_resolve(g_jit, pc);
}

GuestFn jit_wait(u64 pc) { return g_jit ? snx_jit_wait(g_jit, pc) : NULL; }

const char* jit_failure(void) { return g_jit ? "the JIT could not compile it" : g_jit_error; }

int jit_code_added(u64 base, u64 text_size, u64 size) {
    SnxCodeImage image;
    if (!g_jit) return -1;
    memset(&image, 0, sizeof(image));
    image.image = GP(base);
    image.image_size = (size_t)size;
    image.base = base;
    image.text_size = text_size;
    return snx_jit_add_image(g_jit, &image);
}

void jit_code_removed(u64 base) {
    if (g_jit) snx_jit_remove_image(g_jit, base);
}

/* a mapping, permission or write-back change under guest memory (mem.c): what was compiled from the range is stale.
   The call is made with no memory lock held; the JIT's own lock is all the invalidation needs */
void jit_code_invalidate(u64 addr, u64 size) {
    MemBlock block;
    size_t i;
    if (!g_jit) return;
    snx_jit_invalidate(g_jit, addr, size);
    /* a synthetic block the change went through: while it is still executable code its image stays (the invalidation
       dropped its slots and entries, so its next call compiles the bytes as they are now); otherwise it goes */
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < SNX_JIT_IMAGES; ++i) {
        Synthetic* s = &g_jit_synthetic[i];
        if (!s->size || s->base + s->size <= addr || addr + size <= s->base) continue; /* the change is elsewhere */
        if (code_at(s->base, &block)) continue;
        snx_jit_remove_image(g_jit, s->base); /* before the memory can be reused or unmapped */
        s->size = 0;
    }
    pm_unlock(&g_jit_synthetic_lock);
}

void jit_report(void) {
    SnxJitStats s;
    if (!g_jit) return;
    snx_jit_stats(g_jit, &s);
    LOG("jit: %lu batches compiled %lu pcs (%lu failed, %lu still queued, %lu batches dropped by memory changes), %lu "
        "functions, %lu instructions, %lu KiB of code, %.2f s compiling, %lu units loaded from %s\n",
        s.batches, s.compiled, s.failures, s.queued, s.dropped, s.functions, s.instructions,
        (unsigned long)(s.bytes >> 10), s.seconds, s.cached, g_jit_cache);
}
