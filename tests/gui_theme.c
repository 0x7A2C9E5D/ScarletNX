/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GUI themes: the built-ins, a theme.ini over its base, and the themes/ folders the GUI offers. */
#include "common.h"
#include "theme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same(GuiRgb a, GuiRgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
static void parse(GuiTheme* theme, const char* text) { assert(gui_theme_parse(theme, "mine", text, strlen(text))); }
static void write(const char* dir, const char* name, const char* text) {
    char* folder = snx_path_join(dir, name);
    char* file = snx_path_join(folder, "theme.ini");
    assert(!snx_mkdirs(folder));
    if (text) assert(!snx_write_file(file, text, strlen(text)));
    free(file);
    free(folder);
}

int main(void) {
    const GuiTheme *def = gui_theme_builtin("default"), *yorha = gui_theme_builtin("yorha");
    const GuiTheme* loading = gui_theme_builtin("yorha-loading");
    assert(def && yorha && loading && !gui_theme_builtin("nothing") && !gui_theme_builtin(NULL));
    assert(def->run == GUI_RUN_CONSOLE && !def->fade_in_ms && !def->fade_out_ms && def->line_in == GUI_LINE_NONE);
    assert(loading->run == GUI_RUN_STEPS && loading->ring == GUI_RING_RIGHT && loading->texture && loading->bar);
    assert(loading->line_in == GUI_LINE_TYPING && loading->typing_ms == 25 && loading->fade_out_ms == 500);
    assert(!strcmp(loading->title[GUI_SCREEN_LOADING], "BOOTING"));
    assert(def->head == GUI_STYLE_PLAIN && !def->focus_bar && !strcmp(def->marker, "- ") && !def->title[0][0]);
    assert(yorha->head == GUI_STYLE_BAR && yorha->focus_bar && yorha->title[GUI_SCREEN_COMPILING][0]);

    GuiTheme theme;
    parse(&theme, ""); /* nothing: the default, named after its folder */
    assert(same(theme.ink, def->ink) && !strcmp(theme.id, "mine") && !strcmp(theme.name, "mine"));

    /* the base first, wherever it is, then each key over it; what cannot be read keeps the base's */
    parse(&theme, "; a comment\r\n[colors]\r\npage = #0a0B0c\r\nink = #12345\r\nsoft = # 12 34\r\nfocus = #GG0000\r\n"
                  "[theme]\r\nname = Mine\r\nbase = yorha\r\n# another\r\n[head]\r\nstyle = none\r\nrule = maybe\r\n"
                  "loading = \"  BOOT  \"\r\nmenu =\r\n[footer]\r\nstyle = plain\r\nrule = true\r\n"
                  "[body]\r\nmarker = \"* \"\r\nfocus = color\r\n[nowhere]\r\nink = #FFFFFF\r\nstray line\r\n");
    assert(!strcmp(theme.name, "Mine") && !strcmp(theme.id, "mine"));
    assert(same(theme.page, (GuiRgb){0x0A, 0x0B, 0x0C}));
    assert(same(theme.ink, yorha->ink) && same(theme.soft, yorha->soft) && same(theme.focus, yorha->focus));
    assert(theme.head == GUI_STYLE_NONE && theme.head_rule == yorha->head_rule);
    assert(theme.footer == GUI_STYLE_PLAIN && theme.footer_rule && !theme.focus_bar && !strcmp(theme.marker, "* "));
    assert(!strcmp(theme.title[GUI_SCREEN_LOADING], "  BOOT  ") && !theme.title[GUI_SCREEN_MENU][0]);
    assert(!strcmp(theme.title[GUI_SCREEN_COMPILING], yorha->title[GUI_SCREEN_COMPILING]));

    /* the run's layout and its transitions; a time past 10 s, or not a number, keeps the base's */
    parse(&theme, "[theme]\nbase = yorha-loading\n[run]\nlayout = console\nring = left\ntexture = false\nbar = no\n"
                  "[transitions]\nline_in = slide\nline_in_ms = 300\nfade_in_ms = 10001\nfade_out_ms = 5s\n"
                  "typing_ms = 0\nblink_ms = -1\n[colors]\ndim = #010203\n");
    assert(theme.run == GUI_RUN_CONSOLE && theme.ring == GUI_RING_LEFT && !theme.texture && theme.bar);
    assert(theme.line_in == GUI_LINE_SLIDE && theme.line_in_ms == 300 && theme.typing_ms == 0);
    assert(theme.fade_in_ms == loading->fade_in_ms && theme.fade_out_ms == loading->fade_out_ms);
    assert(theme.blink_ms == loading->blink_ms && same(theme.dim, (GuiRgb){1, 2, 3}));

    /* a value longer than its field keeps the base's; an unknown base leaves the default */
    char longer[200] = "[theme]\nbase = elsewhere\n[body]\nmarker = ";
    memset(longer + strlen(longer), 'x', 40);
    parse(&theme, longer);
    assert(!strcmp(theme.marker, def->marker) && same(theme.page, def->page));

    char* dir = snx_temp_dir(NULL);
    assert(dir);
    write(dir, "alpha", "[theme]\nbase = yorha\nname = Alpha\n");
    write(dir, "beta", NULL); /* a folder without theme.ini */
    write(dir, ".hidden", "[theme]\n");
    write(dir, "yorha", "[colors]\npage = #FFFFFF\n"); /* a built-in's name stays the built-in */
    GuiThemes themes = {0};
    gui_themes_scan(&themes, dir);
    assert(themes.count == 4 && !strcmp(themes.ids[0], "default") && !strcmp(themes.ids[1], "yorha") &&
           !strcmp(themes.ids[2], "yorha-loading") && !strcmp(themes.ids[3], "alpha"));
    gui_theme_load(&theme, dir, "alpha");
    assert(!strcmp(theme.name, "Alpha") && same(theme.page, yorha->page));
    gui_theme_load(&theme, dir, "yorha");
    assert(same(theme.page, yorha->page));
    gui_theme_load(&theme, dir, "beta");
    assert(!strcmp(theme.id, "default"));
    gui_themes_free(&themes);
    char* missing = snx_path_join(dir, "missing");
    gui_themes_scan(&themes, missing); /* no themes/ folder: the built-ins */
    assert(themes.count == 3);
    gui_themes_free(&themes);
    free(missing);
    assert(!snx_remove_tree(dir));
    free(dir);
    puts("GUI themes: built-ins, theme.ini over its base, themes/ folders passed");
    return 0;
}
