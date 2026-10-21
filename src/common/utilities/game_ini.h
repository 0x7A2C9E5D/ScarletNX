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

/* addons=: the game's updates and DLCs played with it, their entries (snx_addon_name: a title ID, an update's with
   its version), a comma between two. The most a list holds */
#define SNX_ADDONS_MAX 2048
typedef struct {
    char entry[32]; /* 0100ABCD12341001, 0100ABCD12340800v65536 */
    SnxU64 id;
} SnxAddon;
/* text's entries that are add-ons of the game named game, into list (max of them), each title ID once: an update at
   most, the last one listed, as one runs at a time. Their count; anything else in text is left out */
size_t snx_addons_parse(const char* text, const char* game, SnxAddon* list, size_t max);
/* list as addons= writes it, malloc'd */
char* snx_addons_text(const SnxAddon* list, size_t count);
#endif
