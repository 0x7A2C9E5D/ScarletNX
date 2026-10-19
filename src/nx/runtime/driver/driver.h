/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_DRIVER_H
#define SNX_DRIVER_H
/* -1 leaves the arguments to the runtime; other results are exit codes. */
int snx_driver(int argc, char** argv);
/* 1 when key is a setting of the game's .ini (runtime/options.c); weak, as a build of the driver alone has no runtime
   and so no setting */
int snx_setting_known(const char* key) __attribute__((weak));
/* the link in a game's folder to the dump it was made from, <folder>/<name>.source, owned: what Extract to tmpfs
   extracts again, and what a game made from its dump without extracting it reads its ExeFS and RomFS from */
char* snx_source_link(const char* folder, const char* name);
/* the game's link to dump, made again unless it names it already; a note when the system makes none (Windows without
   the right to) */
void snx_link_source(const char* folder, const char* name, const char* dump);
#endif
