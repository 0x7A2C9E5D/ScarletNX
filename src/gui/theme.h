/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_THEME_H
#define SNX_GUI_THEME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} GuiRgb;
typedef enum { GUI_STYLE_PLAIN, GUI_STYLE_BAR, GUI_STYLE_NONE } GuiStyle;
typedef enum {
    GUI_SCREEN_MENU,
    GUI_SCREEN_GAMES,
    GUI_SCREEN_KEYS,
    GUI_SCREEN_COMPILING,
    GUI_SCREEN_LOADING,
    GUI_SCREEN_COUNT
} GuiScreen;
typedef struct {
    char id[64], name[64];
    GuiRgb page, ink, soft, focus, active, failure;
    GuiStyle head, footer;
    bool head_rule, footer_rule;
    bool focus_bar; /* the focused line as a band of ink, its text in the page colour */
    char marker[16];
    char title[GUI_SCREEN_COUNT][64];
} GuiTheme;
typedef struct {
    char** ids; /* the built-ins, then each folder of themes/ with a theme.ini */
    size_t count;
} GuiThemes;

/* "default" and "yorha"; NULL for any other id */
const GuiTheme* gui_theme_builtin(const char* id);
/* A theme.ini's text over the built-in its [theme] base names (default when absent): a key it lacks, or cannot
   read, keeps the base's value. Returns false only when out of memory. */
bool gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size);
/* id as a built-in, else themes_dir/id/theme.ini; the default theme when neither can be read */
void gui_theme_load(GuiTheme* theme, const char* themes_dir, const char* id);
void gui_themes_scan(GuiThemes* themes, const char* themes_dir);
void gui_themes_free(GuiThemes* themes);
#endif
