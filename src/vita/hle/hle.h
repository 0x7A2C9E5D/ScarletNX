/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_HLE_H
#define SNX_VITA_HLE_H
#include "vita/runtime/process.h"
#include <stdio.h>

/* An imported function, answered by the runtime: its arguments in the calling thread's registers (r0 to r3, then the
   stack: vita_arg), its result returned into r0 (a 64-bit one's high word put in r1 first). One that waits calls
   vita_wait, and what it returns is dropped: the thread's r0 is set when the wait is met */
typedef uint32_t (*VitaHleFunction)(VitaProcess* p, VitaThread* t);
typedef struct {
    uint32_t nid;
    const char* name;
    VitaHleFunction function;
} VitaHle;

/* argument n of the call (AAPCS: r0 to r3, then the stack's words) */
uint32_t vita_arg(VitaProcess* p, VitaThread* t, unsigned n);
/* the hard-float ABI's: single-precision register Sn, double-precision Dn */
static inline float vita_float(const VitaThread* t, unsigned n) {
    uint32_t bits = (uint32_t)(t->cpu.d[n / 2] >> (n & 1 ? 32 : 0));
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}
static inline double vita_double(const VitaThread* t, unsigned n) {
    double d;
    __builtin_memcpy(&d, &t->cpu.d[n], sizeof d);
    return d;
}
/* a floating-point result, in S0 or D0; the function returns r0 as it was, to keep it */
static inline uint32_t vita_return_float(VitaThread* t, float f) {
    uint32_t bits;
    __builtin_memcpy(&bits, &f, sizeof bits);
    t->cpu.d[0] = (t->cpu.d[0] & ~0xffffffffull) | bits;
    return t->cpu.r[0];
}
static inline uint32_t vita_return_double(VitaThread* t, double d) {
    __builtin_memcpy(&t->cpu.d[0], &d, sizeof d);
    return t->cpu.r[0];
}
/* a 64-bit result: its high word into r1, the low one returned */
static inline uint32_t vita_return64(VitaThread* t, uint64_t value) {
    t->cpu.r[1] = (uint32_t)(value >> 32);
    return (uint32_t)value;
}

/* Calls import index for the current thread */
void vita_hle_call(VitaProcess* p, VitaThread* t, uint32_t index);
/* each module's functions (a NULL-named entry ends each table) */
extern const VitaHle vita_hle_kernel[], vita_hle_io[], vita_hle_ctrl[], vita_hle_display[], vita_hle_audio[],
    vita_hle_misc[], vita_hle_gxm[], vita_hle_libc[], vita_hle_fiber[], vita_hle_ngs[];

/* An imported variable (VitaImage.variables): what its slot (VITA_VARIABLE_SIZE bytes, zero) holds, written once as
   the process starts */
typedef struct {
    uint32_t nid;
    const char* name;
    void (*init)(VitaProcess* p, uint32_t slot);
} VitaHleVariable;
extern const VitaHleVariable vita_hle_libc_variables[];
/* Fills each imported variable's slot; the ones the runtime doesn't know stay zero, logged */
void vita_hle_variables(VitaProcess* p);

/* heap.c: a heap in guest memory over [base, base + size), which must be mapped: its address (base, aligned), 0 when
   too small. Its blocks are 8-byte aligned at least; alloc and realloc return 0 when there is no room */
uint32_t vita_heap_init(VitaProcess* p, uint32_t base, uint32_t size);
uint32_t vita_heap_alloc(VitaProcess* p, uint32_t heap, uint32_t size, uint32_t align);
uint32_t vita_heap_realloc(VitaProcess* p, uint32_t heap, uint32_t address, uint32_t size, uint32_t align);
void vita_heap_free(VitaProcess* p, uint32_t heap, uint32_t address);
/* the bytes a block holds (0 for an address not the heap's), and those the heap has given out */
uint32_t vita_heap_usable(VitaProcess* p, uint32_t heap, uint32_t address);
uint32_t vita_heap_in_use(VitaProcess* p, uint32_t heap);

/* the waits other modules meet (vita/runtime/process.c asks): true when met, with the thread's result set */
bool vita_kernel_wait_try(VitaProcess* p, VitaThread* t, uint64_t now);
bool vita_gxm_wait_try(VitaProcess* p, VitaThread* t, uint64_t now);

/* io.c: the files the game left open */
void vita_io_close_all(VitaProcess* p);
/* sceIoOpen's and sceIoClose's work, for SceLibc's stdio: a descriptor or an error; a descriptor's host file, NULL
   for none */
uint32_t vita_io_open(VitaProcess* p, uint32_t path, uint32_t flags);
uint32_t vita_io_close(VitaProcess* p, uint32_t fd);
FILE* vita_io_file(VitaProcess* p, uint32_t fd);
/* audio.c: the ports' output device; their state is freed with the process */
void vita_audio_free(VitaProcess* p);
/* gxm.c: the display queue's thread asking for its next entry (VITA_SVC_DISPLAY_NEXT), and done with it */
void vita_gxm_display_next(VitaProcess* p, VitaThread* t);
void vita_gxm_display_done(VitaProcess* p, VitaThread* t);
void vita_gxm_free(VitaProcess* p);
/* fiber.c: the running fiber's entry returned: the thread goes on after its sceFiberRun */
void vita_fiber_ended(VitaProcess* p, VitaThread* t);
/* display.c: a vblank: the framebuffer shown, the window's events read. False once the player closed the window */
bool vita_display_vblank(VitaProcess* p);
#endif
