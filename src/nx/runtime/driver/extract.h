/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_EXTRACT_H
#define SNX_EXTRACT_H

#include "common/utilities/common.h"

/* argv contains extraction arguments only, without a program name or --extract. */
int snx_extract(int argc, char** argv, const SnxPaths* paths);
/* The title ID of dump's game, as -x would extract it (the keys where -x finds them), read from its content meta and
   NCA headers alone: what a dump's game folder is named by before it is extracted into it, in title and game both. A
   dump of an update or a DLC alone gives the add-on's own in title, and in game the game its content meta names it
   of (snx_addon_kind(*game, *title) tells which it is). 0, else nonzero, said why */
int snx_extract_title(const char* dump, const SnxPaths* paths, SnxU64* title, SnxU64* game);

#endif
