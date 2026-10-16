/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings: the launcher's own two (libgccjit, gdbstub), then every setting of the game's .ini as the
   runtime lists them (scarletnx --settings, its tables), each a row of the form (editor.h). A setting is the .ini's,
   passed to no run, until one is picked; the picks go to this session's runs as --key=value. */
#ifndef SNX_GUI_SETTINGS_H
#define SNX_GUI_SETTINGS_H
#include <stddef.h>
#include "theme.h"

#ifndef SNX_GUI_HAS_GCCJIT
#define SNX_GUI_HAS_GCCJIT 0
#endif
#define GUI_SETTINGS_OWN  2 /* the launcher's own rows, first: libgccjit and gdbstub */
/* the most numbers a whole-number setting goes round, anisotropy's 0 to 16; one with more is typed */
#define GUI_SETTING_WORDS 17
typedef struct {
    int word;      /* a word row's: the word, 0 the .ini's */
    char text[64]; /* a typed row's: empty, the .ini's */
    char option[136];
    const char* words[GUI_SETTING_WORDS + 2]; /* ".ini", the numbers, NULL */
    char numbers[GUI_SETTING_WORDS][4];
} GuiSetting;
typedef struct {
    GuiThemeKey* keys;  /* the rows: the launcher's, then the runtime's by section */
    GuiSetting* values; /* their values, which the keys' offsets are in */
    int count;          /* in one block with the list the keys' names and help point into */
} GuiSettings;
/* list's settings, a line each (section, key, default, int | float | text, lowest, highest, help, tabs between),
   after the launcher's own; another line is left out. 0, else nonzero when out of memory */
int gui_settings_read(GuiSettings* settings, const char* list);
void gui_settings_free(GuiSettings* settings);
/* the row of key set to value, a word or a text as the row takes: 1 when it did */
int gui_settings_set(GuiSettings* settings, const char* key, const char* value);
/* the value picked for key, NULL for the .ini's or no such row */
const char* gui_settings_value(const GuiSettings* settings, const char* key);
/* argv's options for a run: --key=value for each setting picked; with launch the launcher's too, -S -s for the stub
   and --backend=gcc unless prepared (a game from games/ runs no compiler). Their count, count + 1 at most */
size_t gui_settings_options(GuiSettings* settings, int launch, int prepared, const char** argv);
#endif
