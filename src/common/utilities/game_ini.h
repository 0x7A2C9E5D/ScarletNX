/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A recompiled game's .ini as the driver, the launcher and the runtime share it: where it is, the keys of [Game] the
   driver and the launcher write into it (source=, addons=), and the add-ons addons= checks */
#ifndef SNX_GAME_INI_H
#define SNX_GAME_INI_H
#include "common/utilities/common.h"

/* the game's settings, <name>.ini, where the runtime reads them: in its folder, or in an installed build's config
   folder (snx_user_path). Owned */
char* snx_game_ini(const char* folder, const char* name);
/* key's value in the .ini at path, the last one's, malloc'd: NULL for none, an empty one or no such file */
char* snx_ini_value(const char* path, const char* key);
/* key=value in [Game] of the .ini at path, every older key= taken out and the file made when there is none, left as
   it is when it has the value already. value has no line break. 0, or nonzero with a line saying why */
int snx_ini_record(const char* path, const char* key, const char* value);

/* addons=: the game's updates and DLCs played with it, their title IDs as snx_title_text writes them, a comma between
   two. The most a list holds */
#define SNX_ADDONS_MAX 2048
/* text's IDs that are add-ons of the game named game (snx_addon_name), into ids (max of them), each once and an
   update at most, the last one listed: their count. Anything else in text is left out */
size_t snx_addons_parse(const char* text, const char* game, SnxU64* ids, size_t max);
/* ids as addons= writes them, malloc'd */
char* snx_addons_text(const SnxU64* ids, size_t count);
#endif
