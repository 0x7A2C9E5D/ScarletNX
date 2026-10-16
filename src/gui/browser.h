/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_BROWSER_H
#define SNX_GUI_BROWSER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    char** names; /* basenames, sorted in each folder, not display paths */
    char** paths; /* where each is */
    size_t count;
} GuiGames;
/* what a folder's games are: ready to run (executables; on Android a game's code or a dump), CIA files */
enum { GUI_GAMES_RUN = 1, GUI_GAMES_CIA = 2 };
/* A folder's games of kinds added after those listed: its own files only, never its folders. False, error saying why,
   when it cannot be read */
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size);
/* games/'s alone: immediate executable files only, or with cia CIA files (case-insensitive extension, no execute bit
   needed). Never recurse into data/save directories. */
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size);
void gui_games_free(GuiGames* games);
/* The bytes of the files under path, in its folders too, links not followed: 0 for no such folder. It stops, with what
   it counted, once *stop is set by another thread */
uint64_t gui_tree_size(const char* path, const int* stop);
/* An NRO, XCI or NSP (or a decrypted one) by its name: a dump the driver prepares, not a game ready to run */
bool gui_is_dump(const char* name);
/* A 3DS CIA by its name */
bool gui_is_cia(const char* name);
#endif
