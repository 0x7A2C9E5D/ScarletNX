/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/browser.h"
#include "common/utilities/common.h"
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    GuiGames games = {0};
    char error[256];
    char* dir = snx_temp_dir(NULL);
    const char* files[] = {"Alpha.CIA",    "game with spaces.cia", ".hidden.cia",
                           "wrong.cia.so", "launch.exe",           "note.txt"};
    assert(dir);
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
        char* path = snx_path_join(dir, files[i]);
        assert(!snx_write_file(path, "fixture", 7));
        free(path);
    }
    char* subdir = snx_path_join(dir, "directory.cia");
    assert(!snx_mkdirs(subdir));
    /* the space a folder takes: its files' bytes, its folders' too; a link is not followed; a stop ends the count */
    {
        int stop = 0;
        char* deep = snx_path_join(subdir, "deep.bin");
        char* elsewhere = snx_temp_dir(NULL);
        char* big = snx_path_join(elsewhere, "big.bin");
        assert(!snx_write_file(deep, "123", 3) && !snx_write_file(big, "0123456789", 10));
        assert(gui_tree_size(dir, &stop) == 6 * 7 + 3);
#ifndef _WIN32
        char* link = snx_path_join(dir, "link");
        assert(!symlink(elsewhere, link) && gui_tree_size(dir, &stop) == 6 * 7 + 3);
        assert(!remove(link));
        free(link);
#endif
        assert(gui_tree_size(elsewhere, &stop) == 10 && !gui_tree_size("/nonexistent/snx-tree", &stop));
        assert(!gui_tree_size("", &stop)); /* none, not the working folder (Windows lists that for "*") */
        stop = 1;
        assert(!gui_tree_size(dir, &stop));
        assert(!snx_remove_tree(deep) && !snx_remove_tree(elsewhere));
        free(big);
        free(elsewhere);
        free(deep);
    }
    assert(gui_games_scan(&games, dir, true, error, sizeof error) && !error[0]);
    assert(games.count == 2 && !strcmp(games.names[0], "Alpha.CIA") && !strcmp(games.names[1], "game with spaces.cia"));
    /* each with where it is */
    {
        char* alpha = snx_path_join(dir, "Alpha.CIA");
        assert(!strcmp(games.paths[0], alpha));
        free(alpha);
    }
    /* more folders' games go after the ones listed, each with its path; one that is not there is named */
    {
        char* more = snx_temp_dir(NULL);
        char* beta = snx_path_join(more, "beta.cia");
        assert(more && !snx_write_file(beta, "fixture", 7));
        assert(gui_games_add(&games, more, GUI_GAMES_CIA, error, sizeof error) && !error[0] && games.count == 3);
        assert(!strcmp(games.names[2], "beta.cia") && !strcmp(games.paths[2], beta));
        assert(gui_games_add(&games, more, GUI_GAMES_RUN, error, sizeof error) && games.count == 3); /* no CIA */
        assert(!gui_games_add(&games, "/nonexistent/snx-games", GUI_GAMES_CIA, error, sizeof error));
        assert(strstr(error, "/nonexistent/snx-games") && games.count == 3);
        assert(!snx_remove_tree(more));
        free(beta);
        free(more);
    }
    /* Repeated scans replace the old list; neither recursion nor execute bits. */
    assert(gui_games_scan(&games, subdir, true, error, sizeof error) && !games.count);
    assert(!snx_remove_tree(dir));
    assert(!gui_games_scan(&games, dir, true, error, sizeof error) && error[0] && !games.count);
    gui_games_free(&games);
    free(subdir);
    free(dir);
    puts("GUI CIA browser filtering, sorting and refresh passed");
    return 0;
}
