/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared Maxwell program length, dual vertex merge and control stack resolution. */
#include "gpu/shader/translator.h"
#include <stdlib.h>
#include <string.h>

int shader_vk_set(int stage) { return stage == ST_FS ? 1 : (stage == ST_VS || stage == ST_CS) ? 0 : 2; }
u32 bra_target(u64 w, u32 idx) {
    i32 off = (i32)(B(w, 20, 24) << 8) >> 8;
    u32 t = (u32)((i32)idx + 1 + off / 8);
    if (t >= 10 && (t - 10) % 4 == 0) t++;
    return t;
}
int flow_always(u64 w) { return B(w, 16, 3) == 7 && !B(w, 19, 1) && B(w, 0, 5) == 15; }
static int is_term(int id) {
    return id == O_BRA || id == O_BRX || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT || id == O_RET ||
           id == O_JMP;
}
static void mark(Ctx* c, u32 i) {
    if (i < 65536) c->leader[i >> 3] |= 1 << (i & 7);
}
static int is_leader(Ctx* c, u32 i) { return i < 65536 && (c->leader[i >> 3] >> (i & 7)) & 1; }
static void mark_tgt(Ctx* c, u32 i) { mark(c, i); }

int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
int shader_brx_targets(const u64* w, u32 i, u32 n, u32* out, int max);
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len) {
    if (!ops_ready) ops_init();
    const u64* w = (const u64*)code;
    u32 n = max_bytes / 8;
    u32 i = 11, last = 11;
    /* linear scan until an unconditional EXIT past every branch target */
    u32 maxt = 0;
    int unresolved_brx = 0, has_cal = 0;
    for (; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(w[i]);
        last = i;
        if (!op) continue;
        if (op->id == O_BRA || op->id == O_CAL || op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 t = bra_target(w[i], i);
            if (t > maxt) maxt = t;
            if (op->id == O_CAL) has_cal = 1;
        }
        if (op->id == O_BRX) { /* its jump table's targets count too */
            u32 tg[64];
            int nt = shader_brx_targets(w, i, n, tg, 64);
            if (nt < 0) unresolved_brx = 1;
            for (int q = 0; q < nt; q++)
                if (tg[q] > maxt) maxt = tg[q];
        }
        if (!unresolved_brx && op->id == O_EXIT && flow_always(w[i]) && i >= maxt) break;
        if (!unresolved_brx && has_cal && op->id == O_RET && flow_always(w[i]) && i >= maxt) break;
        if (!unresolved_brx && op->id == O_BRA && flow_always(w[i]) && i >= maxt && bra_target(w[i], i) <= i) break;
        if (i > 16384) break;
    }
    u32 len = (last + 1) * 8;
    if (len > max_bytes) len = max_bytes;
    if (out_len) *out_len = len;
    u64 h = 1469598103934665603ull;
    for (u32 k = 0; k < len; k++) {
        h ^= code[k];
        h *= 1099511628211ull;
    }
    return h;
}

/* VertexA + VertexB (NVN dual vertex programs, slot 0 + slot 1): one program that runs A then B like the hardware.
   A's bundles go first and its EXITs become branches to B's first bundle; branches are relative so both keep working.
   B's header is used (declarations come from what the code touches). 0 if it doesn't fit in max_bytes */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes) {
    if (!ops_ready) ops_init();
    u32 la, lb;
    shader_hash(a, max_bytes, &la);
    shader_hash(b, max_bytes, &lb);
    u32 bstart = 10 + ((la / 8 - 10 + 3) / 4) * 4; /* first bundle after A's last one */
    if ((size_t)bstart * 8 + (lb - 80) > max_bytes) return 0;
    memset(out, 0, max_bytes);
    memcpy(out, b, 80);
    memcpy(out + 80, a + 80, (size_t)bstart * 8 - 80);
    memcpy(out + (size_t)bstart * 8, b + 80, lb - 80);
    u64* w = (u64*)out;
    for (u32 i = 11; i < bstart; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(w[i]);
        if (!op || op->id != O_EXIT) continue;
        i32 off = ((i32)(bstart + 1) - (i32)(i + 1)) * 8; /* BRA keeps the EXIT's predicate and flow test */
        w[i] = 0xE240000000000000ull | ((u64)((u32)off & 0xFFFFFF) << 20) | (w[i] & (0xFull << 16)) | (w[i] & 31);
    }
    return 1;
}

/* BRX targets from its jump table (LDC nearby -> cbuf). -1 if we can't tell, or a target is past the n words */
int shader_brx_targets(const u64* w, u32 i, u32 n, u32* out, int max) {
    if (!g_shader_cb_read) return -1;
    u32 ra = B(w[i], 8, 8);
    i32 imm = (i32)(B(w[i], 20, 24) << 8) >> 8;
    for (u32 k = 1; k <= 8 && k < i; k++) {
        u32 j = i - k;
        if (!IDX_OK(j)) continue;
        const Op* o2 = decode(w[j]);
        if (!o2 || o2->id != O_LDC || B(w[j], 0, 8) != ra) continue;
        u32 cidx = B(w[j], 36, 5);
        i32 coff = (i32)(B(w[j], 20, 16) << 16) >> 16;
        int cnt = 0, seen = 0;
        for (u32 e = 0; e < 64 && cnt < max; e++) {
            u32 v;
            if (!g_shader_cb_read(cidx, (u32)(coff + (i32)e * 4), &v)) break;
            i32 bb = (i32)v + imm;
            if (bb < 0 || (bb & 7)) break;
            u32 t = i + 1 + (u32)(bb / 8);
            if (t >= 10 && ((t - 10) & 3) == 0) t++;
            if (t < 11) break;
            seen++;
            if (t >= n) return -1;
            out[cnt++] = t;
        }
        return seen ? cnt : -1;
    }
    return -1;
}
/* ---- static SSY/PBK/PCNT resolution ----
   The hardware has one CRS stack: SSY/PBK/PCNT push a token of their kind with the target, SYNC/BRK/CONT pop the
   entries down to the nearest token of theirs (a BRK out of an SSY region drops the SSY entry). Walk the CFG carrying
   it. If every reachable block sees the same stack on all incoming paths, each SYNC/BRK/CONT has a fixed target and
   we can drop the stack. Any conflict (or no entry to pop, or depth > 16) falls back to the dynamic version. */
typedef struct {
    u8 n;
    u8 kind[16]; /* CRS_KIND */
    u32 s[16];
} Stk;
static int stk_eq(const Stk* a, const Stk* b) {
    return a->n == b->n && !memcmp(a->kind, b->kind, a->n) && !memcmp(a->s, b->s, a->n * 4);
}
typedef struct {
    Ctx* c;
    u32 n;
    int fwd;
    int* lid;
    Stk* st;
    u8* has;
    int* wl;
    int nwl;
} StkA;
/* edge i -> tg, 0 on conflict */
static int stk_succ(StkA* A, u32 i, u32 tg, const Stk* s) {
    if (A->fwd && tg <= i) return 1;                  /* linear mode: no later block -> return */
    if (tg >= A->n || !is_leader(A->c, tg)) return 1; /* no block -> return */
    int L = A->lid[tg];
    if (A->has[L]) return stk_eq(&A->st[L], s);
    A->st[L] = *s;
    A->has[L] = 1;
    A->wl[A->nwl++] = L;
    return 1;
}
static int stk_resolve(Ctx* c, u32 n, int fwd) {
    int nl = 0;
    for (u32 i = 11; i < n; i++)
        if (IDX_OK(i) && is_leader(c, i)) nl++;
    StkA A = {c, n, fwd};
    A.lid = malloc(sizeof(int) * (n + 1));
    u32* ldr = malloc(sizeof(u32) * (nl + 1));
    A.st = malloc(sizeof(Stk) * (nl + 1));
    A.has = calloc(nl + 1, 1);
    A.wl = malloc(sizeof(int) * (nl + 1));
    int k = 0;
    for (u32 i = 11; i < n; i++)
        if (IDX_OK(i) && is_leader(c, i)) {
            A.lid[i] = k;
            ldr[k++] = i;
        }
    c->pop_tgt = calloc(n + 1, sizeof(u32));
    int ok = 1;
    Stk s0;
    memset(&s0, 0, sizeof s0);
    A.st[0] = s0;
    A.has[0] = 1;
    A.wl[A.nwl++] = 0;
    while (ok && A.nwl) {
        int L = A.wl[--A.nwl];
        Stk s = A.st[L];
        for (u32 i = ldr[L]; i < n && ok; i++) {
            if (!IDX_OK(i)) continue;
            if (i != ldr[L] && is_leader(c, i)) {
                ok = stk_succ(&A, i - 1, i, &s);
                break;
            } /* fall through */
            const Op* op = decode(c->w[i]);
            if (!op) continue;
            u64 w = c->w[i];
            int id = op->id;
            u32 pg = B(w, 16, 3), pn = B(w, 19, 1);
            int always = flow_always(w);
            int kk = CRS_KIND(id);
            if (id == O_SSY || id == O_PBK || id == O_PCNT) {
                if (s.n >= 16) {
                    ok = 0;
                    break;
                }
                s.kind[s.n] = (u8)kk;
                s.s[s.n++] = bra_target(w, i);
                continue;
            } /* unpredicated */
            if (pg == 7 && pn) continue; /* never runs */
            if (id == O_SYNC || id == O_BRK || id == O_CONT) {
                int j = s.n;
                while (j > 0 && s.kind[j - 1] != kk) j--;
                if (!j) {
                    ok = 0;
                    break;
                }
                u32 tg = s.s[j - 1];
                if (c->pop_tgt[i] && c->pop_tgt[i] != tg) {
                    ok = 0;
                    break;
                }
                c->pop_tgt[i] = tg;
                Stk s2 = s;
                s2.n = (u8)(j - 1);
                ok = stk_succ(&A, i, tg, &s2);
                if (always) break;
                continue;
            }
            if (id == O_BRA) {
                ok = stk_succ(&A, i, bra_target(w, i), &s);
                if (always) break;
                continue;
            }
            if (id == O_BRX) {
                u32 targets[64];
                int nt = shader_brx_targets(c->w, i, n, targets, 64);
                if (nt < 0) {
                    ok = 0;
                    break;
                }
                for (int q = 0; q < nt && ok; q++) ok = stk_succ(&A, i, targets[q], &s);
                if (always) break;
                continue;
            }
            if (id == O_EXIT && always) break;
        }
    }
    free(A.lid);
    free(ldr);
    free(A.st);
    free(A.has);
    free(A.wl);
    return ok;
}

int shader_resolve_crs(const u64* words, u32 count, u32* pop_targets) {
    if (!words || !pop_targets || count < 12 || count > 65536) return 0;
    Ctx* c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->w = words;
    int unresolved = 0;
    mark(c, 11);
    for (u32 i = 11; i < count; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(words[i]);
        if (!op) continue;
        int id = op->id;
        if (id == O_BRA || id == O_SSY || id == O_PBK || id == O_PCNT) mark_tgt(c, bra_target(words[i], i));
        if (id == O_BRX) {
            u32 targets[64];
            int n = shader_brx_targets(words, i, count, targets, 64);
            if (n < 0) {
                unresolved = 1;
                break;
            }
            for (int j = 0; j < n; j++) {
                u32 target = targets[j];
                mark(c, target);
            }
        }
        if (is_term(id)) {
            u32 next = i + 1;
            while (next < count && !IDX_OK(next)) next++;
            mark(c, next);
        }
    }
    int ok = !unresolved && stk_resolve(c, count, 0);
    if (ok) memcpy(pop_targets, c->pop_tgt, (size_t)count * sizeof *pop_targets);
    free(c->pop_tgt);
    free(c);
    return ok;
}
