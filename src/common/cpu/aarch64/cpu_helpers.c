/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate-a64.c), LGPL-2.1-or-later:
   Copyright (c) 2013 Alexander Graf <agraf@suse.de> */
/* The out-of-line helpers of the generated-code ABI (cpu.h), compiled into every game module's runtime library
   (llvm/rtlib.c), into the gnuabi execution path's generated C and into the tests, after cpu.h. Generated code calls
   snx_fp_*, snx_cas128, snx_mrs/snx_msr and snx_crc32 by name; the templates' C calls the rest. They are plain
   portable C: the library is built for no particular host CPU. */
#include "cpu.h"

#include "helpers/fp.c"
#include "helpers/integer.c"
#include "helpers/crypto.c"
#ifdef SNX_A64_REFERENCE
/* the C reference interpreter still runs the SIMD words without emitters; game modules lower them to IR */
#include "helpers/simd.c"
#include "helpers/simd_mem.c"
#endif
