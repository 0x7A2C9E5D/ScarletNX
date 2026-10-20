/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RtAPI.cas128 (interface.h): STXP's 16-byte compare-and-swap, for the modules of the hosts that have no such
   instruction on every machine (RISC-V before Zacas, LoongArch before v1.1) */
#include "common/runtime/interface.h"
#if defined(__riscv)
#include <asm/hwprobe.h> /* riscv_hwprobe: whether every core has Zacas */
#include <sys/syscall.h>
#include <unistd.h>
#endif

__extension__ typedef unsigned __int128 snx_cas_u128;

/* The host's instruction: amocas.q on RISC-V, LL.D and SC.Q on LoongArch, cmpxchg16b (this file is built with -mcx16,
   src/common/CMakeLists.txt: the attribute alone was inlined into a libatomic call), LDAXP/STLXP and lqarx/stqcx.
   elsewhere. Clang warns that 16 bytes may become a libatomic call; a call would fail the link, as nothing here links
   libatomic */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#if defined(__loongarch__)
__attribute__((target("scq")))
#endif
static uint32_t cas_native(uint64_t* p, uint64_t o0, uint64_t o1, uint64_t n0, uint64_t n1) {
#if defined(__riscv)
    /* LLVM makes no amocas.q of a 16-byte __atomic. Its rd and rs2 are even-odd register pairs, the low half in the
       even one */
    register uint64_t lo __asm__("a2") = o0;
    register uint64_t hi __asm__("a3") = o1;
    register uint64_t new_lo __asm__("a4") = n0;
    register uint64_t new_hi __asm__("a5") = n1;
    __asm__ __volatile__(".option push\n\t.option arch, +zacas\n\tamocas.q.aqrl %0, %2, (%4)\n\t.option pop"
                         : "+r"(lo), "+r"(hi)
                         : "r"(new_lo), "r"(new_hi), "r"(p)
                         : "memory");
    return lo == o0 && hi == o1;
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* p[0] is the integer's high half on a big-endian host */
    snx_cas_u128 expected = ((snx_cas_u128)o0 << 64) | o1, desired = ((snx_cas_u128)n0 << 64) | n1;
    return __atomic_compare_exchange_n((snx_cas_u128*)p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
    snx_cas_u128 expected = ((snx_cas_u128)o1 << 64) | o0, desired = ((snx_cas_u128)n1 << 64) | n0;
    return __atomic_compare_exchange_n((snx_cas_u128*)p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
}
#pragma clang diagnostic pop

/* whether this machine has cas_native's instruction: RISC-V's Zacas as the kernel reports it for every core, and
   LoongArch's SCQ, bit 30 of CPUCFG word 2 (LLVM's host detection reads the same) */
static int native_cas(void) {
#if defined(__riscv)
    struct riscv_hwprobe probe;
    probe.key = RISCV_HWPROBE_KEY_IMA_EXT_0;
    probe.value = 0;
    return syscall(__NR_riscv_hwprobe, &probe, 1, 0, NULL, 0) == 0 && (probe.value & RISCV_HWPROBE_EXT_ZACAS) != 0;
#elif defined(__loongarch__)
    uint32_t word = 2;
    __asm__ __volatile__("cpucfg %0, %0" : "+r"(word));
    return (word >> 30) & 1;
#else
    return 1;
#endif
}

#define CAS_STRIPES 64
static uint32_t g_stripes[CAS_STRIPES]; /* a spin lock for each 16-byte stripe of the address space */

uint32_t snx_runtime_cas128_locked(uint64_t* p, uint64_t o0, uint64_t o1, uint64_t n0, uint64_t n1) {
    uint32_t* lock = &g_stripes[((uintptr_t)p >> 4) % CAS_STRIPES];
    uint64_t seen = o0;
    uint32_t stored = 0;
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {}
    if (__atomic_compare_exchange_n(p, &seen, n0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        seen = o1;
        stored = __atomic_compare_exchange_n(p + 1, &seen, n1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        seen = n0;
        /* a compare-and-swap too: a store another thread made to the low half in between stays */
        if (!stored) __atomic_compare_exchange_n(p, &seen, o0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    return stored;
}

uint32_t snx_runtime_cas128(uint64_t* p, uint64_t o0, uint64_t o1, uint64_t n0, uint64_t n1) {
    static int g_native = -1; /* unknown until the first call; threads that race to it find the same */
    int native = __atomic_load_n(&g_native, __ATOMIC_RELAXED);
    if (native < 0) {
        native = native_cas();
        __atomic_store_n(&g_native, native, __ATOMIC_RELAXED);
    }
    return native ? cas_native(p, o0, o1, n0, n1) : snx_runtime_cas128_locked(p, o0, o1, n0, n1);
}
