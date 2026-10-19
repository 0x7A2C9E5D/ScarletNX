// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// snx_demangle (demangle.h): the Itanium ABI's __cxa_demangle, which every C++ runtime the build uses has (libstdc++,
// libc++abi), and which returns a malloc'd string as demangle.h promises
#include "common/utilities/demangle.h"
#include <cxxabi.h>

extern "C" char* snx_demangle(const char* name) {
    int status = 0;
    char* plain = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    return status == 0 ? plain : nullptr;
}
