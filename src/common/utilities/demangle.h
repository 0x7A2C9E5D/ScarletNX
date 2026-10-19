/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's C++ symbol in the form its source wrote it, through the C++ runtime's demangler (demangle.cpp, a library
   of its own so the C targets that don't want a C++ runtime don't get one) */
#ifndef SNX_DEMANGLE_H
#define SNX_DEMANGLE_H
#ifdef __cplusplus
extern "C" {
#endif
/* name ("_ZN2nn3hid18GetNpadJoyHoldTypeEv") demangled, malloc'd, or NULL for a name that isn't a mangled one */
char* snx_demangle(const char* name);
#ifdef __cplusplus
}
#endif
#endif
