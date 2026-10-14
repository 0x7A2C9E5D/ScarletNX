/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* IPC (ipc.c): service objects, sessions, the request and reply helpers, the service registry */
#ifndef SNX_IPC_H
#define SNX_IPC_H
#include "cpu/aarch64/cpu.h"
#include "hle/kernel/kernel.h"
#include <string.h>

#define MAX_BUFS 8
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;
struct SvcObj;
typedef struct IpcCtx {
    KThread* th;
    u8* msg;
    u64 msg_addr;
    u32 type;
    u32 cmd;
    u32 token;
    const u8* raw;
    u32 raw_size; /* raw args after the CMIF header */
    u64 pid;
    int has_pid;
    u32 copy_h[8], move_h[8];
    int ncopy, nmove;
    IpcBuf a[MAX_BUFS], b[MAX_BUFS], w[MAX_BUFS], x[MAX_BUFS], c[MAX_BUFS];
    int na, nb, nw, nx, nc;
    int is_domain;
    u32 domain_obj;
    u32 in_objs[8];
    int nin_objs;
    int tipc;
    /* reply */
    u32 rc;
    u8 out[0x100];
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    struct SvcObj* out_objs[8];
    int nout_objs;
    struct KSession* sess;
} IpcCtx;

typedef struct SvcObj {
    const char* name;
    int (*dispatch)(struct SvcObj* self, IpcCtx* ctx);
    void (*destroy)(struct SvcObj* self);
    int refs;
    void* data;
    u64 u[8];
} SvcObj;

typedef struct KSession {
    KObj o;
    SvcObj* obj;
    int is_domain;
    struct Domain* domain; /* shared by cloned sessions, same as HOS */
} KSession;
typedef struct Domain {
    SvcObj** obj;
    int n, cap, refs;
    PMutex lk;
} Domain;

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t datasize);
void svcobj_ref(SvcObj* o);
void svcobj_unref(SvcObj* o);
u32 session_new(SvcObj* obj);
void ipc_handle(KThread* t, u32 handle, u64 msg_addr);
/* building a reply by hand (the control requests, a service's fallback). What doesn't fit in out[] is dropped, and
   logged once */
void ipc_out_overflow(IpcCtx* c);
static inline int out_fits(IpcCtx* c, u32 n) {
    if (n <= sizeof c->out - c->out_size) return 1;
    ipc_out_overflow(c);
    return 0;
}
static inline void out32(IpcCtx* c, u32 v) {
    if (!out_fits(c, 4)) return;
    memcpy(c->out + c->out_size, &v, 4);
    c->out_size += 4;
}
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
/* what the dispatchers generated from the .swipc files use (tools/swipc/swipc.c). The generator places every raw
   argument inside the request's raw data and every output inside out[] */
static inline void ipc_in(IpcCtx* c, u32 off, void* dst, u32 n) {
    if (off + n <= c->raw_size)
        memcpy(dst, c->raw + off, n);
    else
        memset(dst, 0, n);
}
static inline void ipc_out(IpcCtx* c, u32 off, const void* src, u32 n) { memcpy(c->out + off, src, n); }
static inline IpcBuf ipc_buf(const IpcBuf* list, int n, int i) {
    IpcBuf none = {0, 0, 0};
    return i < n ? list[i] : none;
}
/* an AutoSelect buffer: the client sends it mapped (A or B) or through the pointer buffer (X or C), and the other
   descriptor empty */
static inline IpcBuf ipc_buf_auto(const IpcBuf* map, int nmap, int i, const IpcBuf* ptr, int nptr, int j) {
    IpcBuf b = ipc_buf(map, nmap, i);
    return b.addr ? b : ipc_buf(ptr, nptr, j);
}
SvcObj* ipc_in_obj(IpcCtx* c, int i); /* the request's object i (domains only), with a reference to drop */
void ipc_close(u32 h);                /* a handle a failed request made for its reply */
#define RC(mod, desc) ((mod) | ((desc) << 9))
#define RC_OK         0
int stub_ok(SvcObj* o, IpcCtx* c);            /* success for a command a service doesn't know, logged */
int quiet_ok(SvcObj* o, IpcCtx* c);           /* the same, silent */
int ipc_unknown(SvcObj* o, IpcCtx* c);        /* what the console answers it: sf's UnknownCommandId, logged */
int ipc_warn_once(const char* name, u32 cmd); /* 1 the first time a service command is seen unhandled */
extern char g_ipc_ring[32][80];               /* the last requests, for crash reports */
extern u32 g_ipc_ring_n;

/* service registry */
typedef SvcObj* (*SvcCtor)(void);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);

#endif
