/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime side of the AOT pack: a pc the game module has no static function for is looked up in the module's
   variant table (variants.h), which the AOT build linked in from the profile run's cache. There is no runtime
   compiler and no interpreter: a pc the pack has no variant for ends the game with the reason (rtapi.c's rt_miss) */
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "common/cpu/recompiler/image.h"
#include "common/cpu/recompiler/variants.h"
#include <stdlib.h>
#include <string.h>

static char g_jit_error[512];      /* why there is no table, for the fatal error of the first miss */
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for the variant hash */

/* the text one image covers from the pc that missed: a block the game maps may be large, and the hash reads only the
   words the variant's runs name */
#define JIT_SYNTHETIC_MAX (4u << 20)
/* the most blocks the game can map while it runs (an NRO, hle/services/ldr.c) that the hash can read */
#define JIT_SYNTHETIC_IMAGES 64
static SnxCodeImage g_jit_synthetic[JIT_SYNTHETIC_IMAGES];
static PMutex g_jit_synthetic_lock = PMUTEX_INIT;
static unsigned long g_jit_hits, g_jit_misses;

typedef struct {
    const SnxCodeImage* modules;
    size_t module_count;
    const SnxCodeImage* synthetic;
    size_t synthetic_count;
} VariantImages;

/* reads one guest word from a module's text or a block the game mapped */
static int image_word(void* user, SnxU64 address, SnxU32* word) {
    const VariantImages* images = (const VariantImages*)user;
    size_t i;
    if (address & 3) return -1;
    for (i = 0; i < images->module_count; ++i) {
        const SnxCodeImage* image = &images->modules[i];
        SnxU64 start = image->base + image->text_off;
        if (image->text_size >= 4 && address >= start && address - start <= image->text_size - 4) {
            memcpy(word, image->image + (address - image->base), sizeof(*word));
            return 0;
        }
    }
    for (i = 0; i < images->synthetic_count; ++i) {
        const SnxCodeImage* image = &images->synthetic[i];
        if (image->text_size >= 4 && address >= image->base && address - image->base <= image->text_size - 4) {
            memcpy(word, image->image + (address - image->base), sizeof(*word));
            return 0;
        }
    }
    return -1;
}

static int leader_has(const SnxVariantFunction* function, SnxU64 pc) {
    SnxU32 i;
    for (i = 0; i < function->leader_count; ++i)
        if (function->leaders[i] == pc) return 1;
    return 0;
}

void jit_init(void) {
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
    if (g_variants)
        LOG("jit: %u runtime code variants in the AOT module\n", g_variants->count);
    else
        WARN("jit: the AOT module has no runtime code variants; the first pc the recompiler missed ends the game\n");
    /* mapping, permission and write-back changes drop the slots and images the range's variants filled (mem.c) */
    mem_set_code_invalidate(jit_code_invalidate);
}

GuestFn jit_wait(u64 pc) {
    VariantImages images;
    SnxU64 h1, h2;
    GuestFn result = NULL;
    u32 i;
    if (!g_variants || !g_jit_images) {
        __atomic_add_fetch(&g_jit_misses, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    images.modules = g_jit_images;
    images.module_count = g_mod->nmods;
    images.synthetic = g_jit_synthetic;
    images.synthetic_count = JIT_SYNTHETIC_IMAGES;
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < g_variants->count; ++i) {
        const SnxVariantFunction* function = &g_variants->functions[i];
        if (function->start > pc || !leader_has(function, pc)) continue;
        if (snx_variant_hash(image_word, &images, function->runs, function->run_count, &h1, &h2)) continue;
        if (h1 != function->h1 || h2 != function->h2) continue;
        if (pc >= g_code_lo && pc < g_code_hi) {
            GuestFn expected = NULL;
            __atomic_compare_exchange_n(&g_disp[(pc - g_code_lo) >> 2], &expected, function->fn, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED);
        }
        result = function->fn;
        break;
    }
    pm_unlock(&g_jit_synthetic_lock);
    if (result)
        __atomic_add_fetch(&g_jit_hits, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&g_jit_misses, 1, __ATOMIC_RELAXED);
    return result;
}

GuestFn jit_resolve(u64 pc) {
    if (pc >= g_code_lo && pc < g_code_hi) {
        GuestFn code = __atomic_load_n(&g_disp[(pc - g_code_lo) >> 2], __ATOMIC_ACQUIRE);
        if (code) return code;
    }
    return jit_wait(pc);
}

/* whether pc is in executable guest memory, and the block it is in */
static int code_at(u64 pc, MemBlock* block) {
    if (pc & 3) return 0;
    mem_query(pc, block);
    return block->state != MS_FREE && (block->perm & PERM_X) && block->size >= 4 && pc >= block->addr &&
           pc - block->addr < block->size;
}

/* whether a module image or a block the game mapped already covers pc */
static int image_covers(u64 pc) {
    size_t i;
    for (i = 0; i < g_mod->nmods; ++i) {
        const SnxCodeImage* image = &g_jit_images[i];
        SnxU64 start = image->base + image->text_off;
        if (pc >= start && pc - start < image->text_size) return 1;
    }
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i) {
        const SnxCodeImage* image = &g_jit_synthetic[i];
        if (image->text_size && pc >= image->base && pc - image->base < image->text_size) return 1;
    }
    return 0;
}

/* the image for the block pc is in, unless one already covers it; 1 when resolve can look pc up */
static int jit_add_synthetic(u64 pc) {
    MemBlock block;
    SnxCodeImage image;
    u64 text;
    size_t i, slot = JIT_SYNTHETIC_IMAGES;
    if (!code_at(pc, &block)) return 0;
    pm_lock(&g_jit_synthetic_lock);
    if (image_covers(pc)) {
        pm_unlock(&g_jit_synthetic_lock);
        return 1;
    }
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i) {
        if (!g_jit_synthetic[i].text_size) {
            slot = i;
            break;
        }
    }
    if (slot == JIT_SYNTHETIC_IMAGES) {
        pm_unlock(&g_jit_synthetic_lock);
        return 0; /* every image slot is a block the game mapped: this pc can't be looked up */
    }
    text = block.addr + block.size - pc;
    if (text > JIT_SYNTHETIC_MAX) text = JIT_SYNTHETIC_MAX;
    memset(&image, 0, sizeof(image));
    image.image = GP(pc);
    image.image_size = (size_t)text;
    image.base = pc;
    image.text_size = text;
    g_jit_synthetic[slot] = image;
    pm_unlock(&g_jit_synthetic_lock);
    return 1;
}

GuestFn jit_resolve_or_add(u64 pc) {
    GuestFn code;
    if (!g_variants || !g_jit_images) return NULL;
    code = jit_resolve(pc);
    if (code) return code;
    if (!jit_add_synthetic(pc)) return NULL;
    return jit_resolve(pc);
}

const char* jit_failure(void) { return g_jit_error[0] ? g_jit_error : "the AOT pack has no code for it"; }

int jit_code_added(u64 base, u64 text_size, u64 size) {
    SnxCodeImage image;
    size_t i, slot = JIT_SYNTHETIC_IMAGES;
    if (!g_variants || !g_jit_images) return -1;
    memset(&image, 0, sizeof(image));
    image.image = GP(base);
    image.image_size = (size_t)size;
    image.base = base;
    image.text_size = text_size;
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i) {
        if (!g_jit_synthetic[i].text_size) {
            slot = i;
            break;
        }
    }
    if (slot == JIT_SYNTHETIC_IMAGES) {
        pm_unlock(&g_jit_synthetic_lock);
        return -1;
    }
    g_jit_synthetic[slot] = image;
    pm_unlock(&g_jit_synthetic_lock);
    return 0;
}

void jit_code_removed(u64 base) {
    size_t i;
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i)
        if (g_jit_synthetic[i].text_size && g_jit_synthetic[i].base == base) g_jit_synthetic[i].text_size = 0;
    pm_unlock(&g_jit_synthetic_lock);
}

/* a mapping, permission or write-back change under guest memory (mem.c): the slots the range's variants filled are
   stale, and a block the game mapped stops being code. The call is made with no memory lock held */
void jit_code_invalidate(u64 addr, u64 size) {
    MemBlock block;
    u64 change_lo = addr, change_hi = size > ~(u64)0 - addr ? ~(u64)0 : addr + size;
    u64 lo = change_lo < g_code_lo ? g_code_lo : change_lo;
    u64 hi = change_hi > g_code_hi ? g_code_hi : change_hi;
    u64 at;
    size_t i;
    if (!g_jit_images) return;
    if (lo & 3) lo = (lo + 3) & ~(u64)3;
    for (at = lo; at < hi; at += 4)
        __atomic_store_n(&g_disp[(at - g_code_lo) >> 2], NULL, __ATOMIC_RELEASE);
    pm_lock(&g_jit_synthetic_lock);
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i) {
        SnxCodeImage* image = &g_jit_synthetic[i];
        if (!image->text_size || image->base + image->text_size <= change_lo || change_hi <= image->base) continue;
        if (code_at(image->base, &block)) continue;
        image->text_size = 0; /* before the memory can be reused or unmapped */
    }
    pm_unlock(&g_jit_synthetic_lock);
}

void jit_report(void) {
    unsigned long images = g_mod ? g_mod->nmods : 0;
    size_t i;
    for (i = 0; i < JIT_SYNTHETIC_IMAGES; ++i)
        if (g_jit_synthetic[i].text_size) ++images;
    LOG("jit: %u AOT variants, %lu hits, %lu misses, %lu images\n", g_variants ? g_variants->count : 0,
        __atomic_load_n(&g_jit_hits, __ATOMIC_RELAXED), __atomic_load_n(&g_jit_misses, __ATOMIC_RELAXED), images);
}
