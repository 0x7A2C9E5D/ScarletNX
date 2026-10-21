/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GDB's JIT interface (gdb_jit.h), as GDB's manual ("JIT Compilation Interface") defines it: a descriptor GDB finds by
   name, holding a list of in-memory object files, and an empty function GDB breaks on, called after each change with
   the entry that changed. An object is a load's relocatable object with the loaded sections' sh_addr set to where the
   load put them, so GDB's reader takes their symbols at those addresses (an ET_REL symbol is its section's address
   plus st_value) and applies the object's relocations to .eh_frame itself */
#include "common/cpu/recompiler/llvm/gdb_jit.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The names, layout and version are GDB's, which reads them from the process's memory */
enum { JIT_NOACTION, JIT_REGISTER_FN, JIT_UNREGISTER_FN };
struct jit_code_entry {
    struct jit_code_entry* next_entry;
    struct jit_code_entry* prev_entry;
    const char* symfile_addr;
    uint64_t symfile_size;
};
struct jit_descriptor {
    uint32_t version;
    uint32_t action_flag;
    struct jit_code_entry* relevant_entry;
    struct jit_code_entry* first_entry;
};
#ifdef _WIN32
/* the GDB stub is UNIX only (nx/runtime/gdbstub.c), and the loader's objects are COFF here: nothing is registered */
void snx_gdb_jit_enable(int on) { (void)on; }
void snx_gdb_jit_remove(const void* owner) { (void)owner; }
#else
#include <pthread.h>

/* Weak, so a program that links the recompiler library with LLVM's own GDB listener (which defines them too) has one
   descriptor, as the interface requires. The breakpoint's call must stay, out of line, for GDB to stop at */
void __jit_debug_register_code(void);
extern struct jit_descriptor __jit_debug_descriptor;
__attribute__((weak, noinline)) void __jit_debug_register_code(void) { __asm__ __volatile__(""); }
__attribute__((weak)) struct jit_descriptor __jit_debug_descriptor = {1, JIT_NOACTION, NULL, NULL};

/* An entry and its object, one allocation: GDB reads the jit_code_entry at its start */
typedef struct {
    struct jit_code_entry entry;
    const void* owner;
} Entry;

static struct {
    pthread_mutex_t lock; /* loaders load on different threads, and the list is the process's */
    int on;
} g_gdb_jit = {PTHREAD_MUTEX_INITIALIZER, 0};

void snx_gdb_jit_enable(int on) { g_gdb_jit.on = on; }

/* under the lock */
static void notify(struct jit_code_entry* entry, uint32_t action) {
    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = action;
    __jit_debug_register_code();
    __jit_debug_descriptor.relevant_entry = NULL;
    __jit_debug_descriptor.action_flag = JIT_NOACTION;
}

void snx_gdb_jit_register(const void* owner, const SnxElfObject* o) {
    size_t size = o->input->size, i;
    unsigned char* object;
    Entry* e;
    if (!g_gdb_jit.on) return;
    e = (Entry*)malloc(sizeof(Entry) + size);
    if (!e) return;
    object = (unsigned char*)(e + 1);
    memcpy(object, o->input->data, size);
    for (i = 0; i < o->eh->e_shnum; ++i) {
        Elf64_Shdr sh;
        if (o->kind[i] < 0) continue;
        memcpy(&sh, object + o->eh->e_shoff + i * sizeof(sh), sizeof(sh));
        sh.sh_addr = o->address[i];
        memcpy(object + o->eh->e_shoff + i * sizeof(sh), &sh, sizeof(sh));
    }
    e->entry.prev_entry = NULL;
    e->entry.symfile_addr = (const char*)object;
    e->entry.symfile_size = size;
    e->owner = owner;
    pthread_mutex_lock(&g_gdb_jit.lock);
    e->entry.next_entry = __jit_debug_descriptor.first_entry;
    if (e->entry.next_entry) e->entry.next_entry->prev_entry = &e->entry;
    __jit_debug_descriptor.first_entry = &e->entry;
    notify(&e->entry, JIT_REGISTER_FN);
    pthread_mutex_unlock(&g_gdb_jit.lock);
}

void snx_gdb_jit_remove(const void* owner) {
    struct jit_code_entry *entry, *next;
    pthread_mutex_lock(&g_gdb_jit.lock);
    for (entry = __jit_debug_descriptor.first_entry; entry; entry = next) {
        next = entry->next_entry;
        if (((Entry*)entry)->owner != owner) continue;
        if (entry->prev_entry)
            entry->prev_entry->next_entry = next;
        else
            __jit_debug_descriptor.first_entry = next;
        if (next) next->prev_entry = entry->prev_entry;
        notify(entry, JIT_UNREGISTER_FN);
        free(entry);
    }
    pthread_mutex_unlock(&g_gdb_jit.lock);
}
#endif
