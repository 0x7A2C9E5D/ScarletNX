/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "browser.h"
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static bool suffix(const char* name, const char* extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n - e + i]) != extension[i]) return false;
    return true;
}
bool gui_is_dump(const char* name) {
    return suffix(name, ".nro") || suffix(name, ".xci") || suffix(name, ".nsp") || suffix(name, ".dxci") ||
           suffix(name, ".dnsp");
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    snx_free_strings(games->paths, games->count);
    games->names = games->paths = NULL;
    games->count = 0;
}
uint64_t gui_tree_size(const char* path, const int* stop) {
    char** names = NULL;
    size_t count = 0, i;
    uint64_t total = 0;
    if (snx_list_dir(path, &names, &count)) return 0;
    for (i = 0; i < count && !__atomic_load_n(stop, __ATOMIC_RELAXED); ++i) {
        char* entry = snx_path_join(path, names[i]);
        if (entry && !snx_is_link(entry))
            total += snx_is_dir(entry) ? gui_tree_size(entry, stop) : snx_file_size(entry);
        free(entry);
    }
    snx_free_strings(names, count);
    return total;
}
/* whether a folder's entry is a game of kinds: a CIA, or one ready to run. Libraries often carry executable bits but
   are not games, and hidden files are not shown. On Android the shared storage runs nothing: a game is <name> beside
   <name>_code.so (name is cut there), and a dump put there, to be prepared, shows with its own name */
static bool playable(const char* directory, char* name, int kinds) {
    char* path;
    bool listed;
    if (name[0] == '.') return false;
    if (suffix(name, ".cia")) {
        if (!(kinds & GUI_GAMES_CIA)) return false;
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        return listed;
    }
    if (!(kinds & GUI_GAMES_RUN)) return false;
#ifdef __ANDROID__
    if (suffix(name, "_code.so")) {
        name[strlen(name) - 8] = 0;
        return true;
    }
    return gui_is_dump(name);
#else
#ifdef _WIN32
    if (!suffix(name, ".exe")) return false;
#else
    if (suffix(name, ".so") || strstr(name, ".so.") || suffix(name, ".dll") || suffix(name, ".dylib")) return false;
#endif
    path = snx_path_join(directory, name);
#ifndef _WIN32
    if (!path || access(path, X_OK)) {
        free(path);
        return false;
    }
#endif
    {
        char* executable = path ? snx_find_tool(path) : NULL; /* regular file, with UTF-8 paths on Windows */
        free(path);
        listed = executable != NULL;
        free(executable);
    }
    return listed;
#endif
}
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size) {
    char **names = NULL, **more;
    size_t count = 0, i;
    if (size) error[0] = 0;
    if (snx_list_dir(directory, &names, &count)) {
        snprintf(error, size, "Cannot open %s.", directory);
        return false;
    }
    more = (char**)realloc(games->names, (games->count + count + 1) * sizeof *more);
    if (more) games->names = more;
    more = more ? (char**)realloc(games->paths, (games->count + count + 1) * sizeof *more) : NULL;
    if (more) games->paths = more;
    if (!more) {
        snx_free_strings(names, count);
        snprintf(error, size, "Not enough memory to list %s.", directory);
        return false;
    }
    /* snx_list_dir already sorts names */
    for (i = 0; i < count; ++i) {
        char* path = playable(directory, names[i], kinds) ? snx_path_join(directory, names[i]) : NULL;
        if (!path) continue;
        games->names[games->count] = names[i];
        games->paths[games->count++] = path;
        names[i] = NULL;
    }
    snx_free_strings(names, count);
    return true;
}
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size) {
    gui_games_free(games);
    if (gui_games_add(games, directory, cia ? GUI_GAMES_CIA : GUI_GAMES_RUN, error, size)) return true;
    if (!strncmp(error, "Cannot open", 11))
        snprintf(error, size, "Cannot open games/. Create it in the displayed working directory.");
    return false;
}
