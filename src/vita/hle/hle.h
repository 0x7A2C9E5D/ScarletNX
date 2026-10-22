/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_HLE_H
#define SNX_VITA_HLE_H
#include "vita/runtime/process.h"

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
/* a 64-bit result: its high word into r1, the low one returned */
static inline uint32_t vita_return64(VitaThread* t, uint64_t value) {
    t->cpu.r[1] = (uint32_t)(value >> 32);
    return (uint32_t)value;
}

/* Calls import index for the current thread */
void vita_hle_call(VitaProcess* p, VitaThread* t, uint32_t index);
/* each module's functions (a NULL-named entry ends each table) */
extern const VitaHle vita_hle_kernel[], vita_hle_io[], vita_hle_ctrl[], vita_hle_display[], vita_hle_audio[],
    vita_hle_misc[], vita_hle_gxm[];

/* the waits other modules meet (vita/runtime/process.c asks): true when met, with the thread's result set */
bool vita_kernel_wait_try(VitaProcess* p, VitaThread* t, uint64_t now);
bool vita_gxm_wait_try(VitaProcess* p, VitaThread* t, uint64_t now);

/* io.c: the files the game left open */
void vita_io_close_all(VitaProcess* p);
/* audio.c: the ports' output device; their state is freed with the process */
void vita_audio_free(VitaProcess* p);
/* gxm.c: the display queue's thread asking for its next entry (VITA_SVC_DISPLAY_NEXT), and done with it */
void vita_gxm_display_next(VitaProcess* p, VitaThread* t);
void vita_gxm_display_done(VitaProcess* p, VitaThread* t);
void vita_gxm_free(VitaProcess* p);
/* display.c: a vblank: the framebuffer shown, the window's events read. False once the player closed the window */
bool vita_display_vblank(VitaProcess* p);
#endif
