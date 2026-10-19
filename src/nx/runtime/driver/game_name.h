/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a recompiled game is called: the name it is shown by, and the name of its files */
#ifndef SNX_GAME_NAME_H
#define SNX_GAME_NAME_H
#include "nx/loader/loader.h"

/* name when given, else the NPDM's when it is not the SDK's "Application", else the input's file name without its
   extension; UTF-8 kept, path delimiters and C/OS metacharacters taken out, "Game" when nothing is left */
char* snx_game_title(const char* name, const SnxNpdm* npdm, const char* input);
/* name when given, else the title ID, 16 hex digits, when the NPDM gives the game one of its own, else its title: the
   same game is the same files whatever its dump was called */
char* snx_game_name(const char* name, const SnxNpdm* npdm, const char* input);
/* what the game is shown as (its window, the recompile's lines, its settings' head): name when given, else the name
   in its control.nacp, else its title */
char* snx_game_shown(const char* name, const SnxProgram* program);
/* the game's settings, <name>.ini, where the runtime reads them: in its folder, or in an installed build's config
   folder (snx_user_path). Owned */
char* snx_game_ini(const char* folder, const char* name);
/* the dump the game was made from, owned: what Extract to tmpfs extracts again, and what a game made from its dump
   without extracting it reads its ExeFS and RomFS from. source= in the game's .ini, else <folder>/<name>.source, the
   link an older version made (Windows makes links only with the right to). recorded, when not NULL: 1 when either
   names a dump, there or not */
char* snx_game_source(const char* folder, const char* name, int* recorded);
/* dump, absolute, as source= in [Game] of the game's .ini, the file made when there is none and left as it is when it
   names dump already. 0, or nonzero with a line saying why */
int snx_record_source(const char* folder, const char* name, const char* dump);
#endif
