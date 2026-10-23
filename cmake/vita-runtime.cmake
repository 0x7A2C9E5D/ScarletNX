# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The helpers compiled blocks call (snx_arm_rt), embedded in the Vita driver's offline compiler, which links each game
# module with them, as cmake/ctr-runtime.cmake does for the 3DS driver's
file(READ "${INPUT}" data HEX)
string(REGEX REPLACE "(..)" "0x\\1," data "${data}")
file(WRITE "${OUTPUT}" "/* Generated from snx_arm_rt; do not edit. */\n#include \"vita/aot/runtime.h\"\nconst unsigned char snx_vita_aot_runtime[] = {${data}};\nconst size_t snx_vita_aot_runtime_size = sizeof snx_vita_aot_runtime;\n")
