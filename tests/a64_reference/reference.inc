/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ================================================================================================
   TEST ONLY. NOT IN PRODUCTION.
   The C reference interpreter's helpers, included by cpu_helpers.c when SNX_A64_REFERENCE is
   defined (the differential tests and the emitted runners). No game module compiles this file,
   no runtime path calls it. crypto.c is included here too: it is production for 32-bit ARM, but
   for A64 it is the reference the LLVM backend is compared against.
   ================================================================================================ */
#include "common/cpu/aarch64/helpers/crypto.c"
#include "integer_reference.c"
#include "simd.c"
#include "simd_mem.c"
