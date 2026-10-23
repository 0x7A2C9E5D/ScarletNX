/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_AOT_RUNTIME_H
#define SNX_VITA_AOT_RUNTIME_H
#include <stddef.h>
/* snx_arm_rt, the helpers compiled blocks call, as an archive the offline compiler links each module with
   (cmake/vita-runtime.cmake makes it from the build's) */
extern const unsigned char snx_vita_aot_runtime[];
extern const size_t snx_vita_aot_runtime_size;
#endif
