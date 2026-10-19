/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/helper-a64.c, translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The integer helpers every generated-code module keeps: the 16-byte compare-and-swap and the system registers EL0
   reaches. The compiler evaluates logical immediates itself, and CRC32 is emitted as IR; both live in
   integer_reference.c for the C reference interpreter. */

/* STXP of two doublewords: one atomic compare-and-swap of all 16 bytes. Swapping the halves one at a time let
   Tears of the Kingdom's lock-free lists pair one thread's pointer with another's counter. Clang emits
   lock cmpxchg16b with cx16 enabled (the builds also pass -mcx16: Clang 18 still called libatomic with the
   attribute alone), LDAXP/STLXP on AArch64, and lqarx/stqcx. on PowerPC64 from POWER8 (cmake/cross.cmake's
   -mcpu=power8); GCC makes it a libatomic call, which a module can't link. */
#ifndef __clang__
#error "the CPU helpers need Clang: GCC turns the 16-byte compare-and-swap into a libatomic call"
#endif
__extension__ typedef unsigned __int128 snx_u128;
#if defined(__x86_64__)
__attribute__((target("cx16")))
#endif
u32 snx_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
    snx_u128 expected = ((snx_u128)o1 << 64) | o0, desired = ((snx_u128)n1 << 64) | n0;
    return __atomic_compare_exchange_n((snx_u128*)p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* o0:op1:CRn:CRm:op2 of an MRS/MSR system register with op0 = 3 */
#define SYSREG(op1, crn, crm, op2) ((1u << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
/* the Cortex-A57's values (TRM DDI0488H): CTR_EL0 for 64-byte lines, DCZID_EL0 for 64-byte DC ZVA blocks */
#define A57_CTR_EL0   0x8444C004u
#define A57_DCZID_EL0 4u
#define CNTFRQ_HZ     19200000u
/* the FPCR bits the A57 implements (AHP, DN, FZ, RMode; the trap enables and FZ16 are RES0, TRM tables 14-3 and
   14-4), and FPSR's N, Z, C, V, QC and cumulative flags */
#define A57_FPCR_MASK 0x07C00000u
#define A57_FPSR_MASK 0xF800009Fu

u64 snx_mrs(CPU* c, u32 sysreg) {
    switch (sysreg) {
        case SYSREG(3, 4, 2, 0): return get_nzcv(c);
        case SYSREG(3, 4, 4, 0): return c->fpcr;
        case SYSREG(3, 4, 4, 1): return c->fpsr;
        case SYSREG(3, 13, 0, 2): return c->tpidr;
        case SYSREG(3, 13, 0, 3): return c->tpidrro;
        case SYSREG(3, 0, 0, 1): return A57_CTR_EL0;
        case SYSREG(3, 0, 0, 7): return A57_DCZID_EL0;
        case SYSREG(3, 14, 0, 0): return CNTFRQ_HZ;
        case SYSREG(3, 14, 0, 1): /* CNTPCT_EL0 */
        case SYSREG(3, 14, 0, 2): /* CNTVCT_EL0 */
            return c->rt && c->rt->ticks ? c->rt->ticks() : 0;
        default: return 0;
    }
}

void snx_msr(CPU* c, u32 sysreg, u64 v) {
    switch (sysreg) {
        case SYSREG(3, 4, 2, 0): set_nzcv(c, (u32)(v >> 28)); break;
        case SYSREG(3, 4, 4, 0):
            c->fpcr = (u32)v & A57_FPCR_MASK;
            if (c->rt && c->rt->fpcr_changed) c->rt->fpcr_changed(c);
            break;
        case SYSREG(3, 4, 4, 1): c->fpsr = (u32)v & A57_FPSR_MASK; break;
        case SYSREG(3, 13, 0, 2): c->tpidr = v; break;
        default: break; /* read-only at EL0 (TPIDRRO_EL0, the ID and counter registers) or not modelled */
    }
}
#undef SYSREG
