/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "theme.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

static const GuiTheme builtins[] = {
    {
        .id = "default",
        .name = "Default",
        .page = {0, 0, 0},
        .ink = {220, 220, 220},
        .soft = {160, 170, 180},
        .focus = {255, 255, 0},
        .active = {255, 85, 255},
        .failure = {255, 0, 0},
        .dim = {80, 80, 80},
        .head = GUI_STYLE_PLAIN,
        .footer = GUI_STYLE_PLAIN,
        .marker = "- ",
        .typing_ms = 25,
        .blink_ms = 500,
    },
    {
        .id = "yorha",
        .name = "YoRHa",
        .page = {0xDA, 0xD6, 0xBE},
        .ink = {0x45, 0x41, 0x3A},
        .soft = {0x8A, 0x86, 0x76},
        .focus = {0x45, 0x41, 0x3A},
        .active = {0x8A, 0x6A, 0x2A},
        .failure = {0xA5, 0x47, 0x3C},
        .dim = {0xB4, 0xB0, 0x9C},
        .head = GUI_STYLE_BAR,
        .footer = GUI_STYLE_BAR,
        .head_rule = true,
        .focus_bar = true,
        .marker = "\xE2\x96\xA0 ", /* U+25A0, the font's square */
        .title = {"SCARLETNX  /  MENU", "SCARLETNX  /  GAMES", "SCARLETNX  /  KEYS", "SYSTEM  /  COMPILING",
                  "SYSTEM  /  LOADING"},
        .fade_in_ms = 250,
        .fade_out_ms = 500,
        .line_in_ms = 220,
        .typing_ms = 25,
        .blink_ms = 500,
    },
    {
        .id = "yorha-loading",
        .name = "YoRHa loading",
        .page = {0x14, 0x14, 0x16},
        .ink = {0xE6, 0xE4, 0xDC},
        .soft = {0x8E, 0x8C, 0x86},
        .focus = {0xE6, 0xE4, 0xDC},
        .active = {0xD9, 0xB2, 0x4A},
        .failure = {0xD2, 0x46, 0x3A},
        .dim = {0x3E, 0x3E, 0x40},
        .head = GUI_STYLE_BAR,
        .footer = GUI_STYLE_PLAIN,
        .footer_rule = true,
        .focus_bar = true,
        .marker = "\xE2\x96\xA0 ",
        .title = {"SCARLETNX  /  MENU", "SCARLETNX  /  GAMES", "SCARLETNX  /  KEYS", "COMPILING", "BOOTING"},
        .run = GUI_RUN_STEPS,
        .texture = true,
        .bar = true,
        .ring = GUI_RING_RIGHT,
        .line_in = GUI_LINE_TYPING,
        .fade_in_ms = 250,
        .fade_out_ms = 500,
        .line_in_ms = 220,
        .typing_ms = 25,
        .blink_ms = 500,
    },
};
static const char* const screens[GUI_SCREEN_COUNT] = {"menu", "games", "keys", "compiling", "loading"};

const GuiTheme* gui_theme_builtin(const char* id) {
    for (size_t i = 0; id && i < sizeof(builtins) / sizeof(*builtins); ++i)
        if (!strcmp(builtins[i].id, id)) return &builtins[i];
    return NULL;
}

static char* trim(char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}
/* "quoted" keeps its spaces */
static char* unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        return s + 1;
    }
    return s;
}
static bool copy(char* out, size_t size, const char* value) {
    if (strlen(value) >= size) return false;
    strcpy(out, value);
    return true;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool colour(GuiRgb* out, const char* value) {
    uint8_t v[3];
    if (strlen(value) != 7 || value[0] != '#') return false;
    for (int i = 0; i < 3; ++i) {
        int hi = hex(value[1 + 2 * i]), lo = hex(value[2 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        v[i] = (uint8_t)(hi * 16 + lo);
    }
    *out = (GuiRgb){v[0], v[1], v[2]};
    return true;
}
static bool style(GuiStyle* out, const char* value) {
    if (!strcmp(value, "plain"))
        *out = GUI_STYLE_PLAIN;
    else if (!strcmp(value, "bar"))
        *out = GUI_STYLE_BAR;
    else if (!strcmp(value, "none"))
        *out = GUI_STYLE_NONE;
    else
        return false;
    return true;
}
static bool flag(bool* out, const char* value) {
    if (!strcmp(value, "true"))
        *out = true;
    else if (!strcmp(value, "false"))
        *out = false;
    else
        return false;
    return true;
}
static bool milliseconds(uint32_t* out, const char* value) {
    char* end;
    unsigned long n = strtoul(value, &end, 10);
    if (!*value || *end || n > 10000) return false;
    *out = (uint32_t)n;
    return true;
}
static bool word(int* out, const char* value, const char* const* words, int count) {
    for (int i = 0; i < count; ++i)
        if (!strcmp(value, words[i])) {
            *out = i;
            return true;
        }
    return false;
}
static void apply(GuiTheme* t, const char* section, const char* key, const char* value) {
    if (!strcmp(section, "theme") && !strcmp(key, "name"))
        copy(t->name, sizeof t->name, value);
    else if (!strcmp(section, "colors")) {
        GuiRgb* roles[] = {&t->page, &t->ink, &t->soft, &t->focus, &t->active, &t->failure, &t->dim};
        static const char* const names[] = {"page", "ink", "soft", "focus", "active", "failure", "dim"};
        for (size_t i = 0; i < sizeof roles / sizeof *roles; ++i)
            if (!strcmp(key, names[i])) colour(roles[i], value);
    } else if (!strcmp(section, "head") || !strcmp(section, "footer")) {
        bool head = section[0] == 'h';
        if (!strcmp(key, "style"))
            style(head ? &t->head : &t->footer, value);
        else if (!strcmp(key, "rule"))
            flag(head ? &t->head_rule : &t->footer_rule, value);
        else
            for (int i = 0; head && i < GUI_SCREEN_COUNT; ++i)
                if (!strcmp(key, screens[i])) copy(t->title[i], sizeof t->title[i], value);
    } else if (!strcmp(section, "body")) {
        if (!strcmp(key, "marker"))
            copy(t->marker, sizeof t->marker, value);
        else if (!strcmp(key, "focus") && (!strcmp(value, "bar") || !strcmp(value, "color")))
            t->focus_bar = value[0] == 'b';
    } else if (!strcmp(section, "run")) {
        static const char *const layouts[] = {"console", "steps"}, *const rings[] = {"none", "left", "right"};
        int n;
        if (!strcmp(key, "layout") && word(&n, value, layouts, 2))
            t->run = (GuiRunLayout)n;
        else if (!strcmp(key, "ring") && word(&n, value, rings, 3))
            t->ring = (GuiRing)n;
        else if (!strcmp(key, "texture"))
            flag(&t->texture, value);
        else if (!strcmp(key, "bar"))
            flag(&t->bar, value);
    } else if (!strcmp(section, "transitions")) {
        static const char* const entrances[] = {"none", "typing", "slide", "fade"};
        int n;
        if (!strcmp(key, "line_in") && word(&n, value, entrances, 4))
            t->line_in = (GuiLineIn)n;
        else if (!strcmp(key, "fade_in_ms"))
            milliseconds(&t->fade_in_ms, value);
        else if (!strcmp(key, "fade_out_ms"))
            milliseconds(&t->fade_out_ms, value);
        else if (!strcmp(key, "line_in_ms"))
            milliseconds(&t->line_in_ms, value);
        else if (!strcmp(key, "typing_ms"))
            milliseconds(&t->typing_ms, value);
        else if (!strcmp(key, "blink_ms"))
            milliseconds(&t->blink_ms, value);
    }
}
/* each key = value of text, with its section, to visit; false when out of memory */
static bool visit(const char* text, size_t size, GuiTheme* theme, bool base_only) {
    char* copy_of = malloc(size + 1);
    if (!copy_of) return false;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    char section[32] = "";
    for (char *line = copy_of, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char* s = trim(line);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end && (size_t)(end - s - 1) < sizeof section) {
                memcpy(section, s + 1, (size_t)(end - s - 1));
                section[end - s - 1] = 0;
            } else
                section[0] = 0;
            continue;
        }
        char* equals = strchr(s, '=');
        if (!equals) continue;
        *equals = 0;
        char *key = trim(s), *value = unquote(trim(equals + 1));
        if (base_only) {
            const GuiTheme* base = !strcmp(section, "theme") && !strcmp(key, "base") ? gui_theme_builtin(value) : NULL;
            if (base) *theme = *base;
        } else
            apply(theme, section, key, value);
    }
    free(copy_of);
    return true;
}
bool gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size) {
    *theme = builtins[0];
    if (!visit(text, size, theme, true)) return false;
    copy(theme->id, sizeof theme->id, id);
    copy(theme->name, sizeof theme->name, id);
    return visit(text, size, theme, false);
}
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id) {
    for (size_t i = 0; i < count; ++i) {
        char* folder = dirs[i] ? snx_path_join(dirs[i], id) : NULL;
        char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
        size_t size = 0;
        unsigned char* text = path ? snx_read_file(path, &size) : NULL;
        bool read = text && gui_theme_parse(theme, id, (const char*)text, size);
        free(text);
        free(path);
        free(folder);
        if (read) return;
    }
    const GuiTheme* builtin = gui_theme_builtin(id);
    *theme = builtin ? *builtin : builtins[0];
}

void gui_themes_free(GuiThemes* themes) {
    snx_free_strings(themes->ids, themes->count);
    themes->ids = NULL;
    themes->count = 0;
}
static bool listed(const GuiThemes* themes, const char* id) {
    for (size_t i = 0; i < themes->count; ++i)
        if (!strcmp(themes->ids[i], id)) return true;
    return false;
}
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count) {
    gui_themes_free(themes);
    size_t total = sizeof(builtins) / sizeof(*builtins), capacity = total;
    themes->ids = calloc(capacity, sizeof *themes->ids);
    if (!themes->ids) return;
    for (size_t i = 0; i < total; ++i) themes->ids[themes->count++] = snx_strdup(builtins[i].id);
    for (size_t d = 0; d < count; ++d) {
        char** names = NULL;
        size_t found = 0;
        if (!dirs[d] || snx_list_dir(dirs[d], &names, &found)) continue; /* no such folder: nothing from it */
        char** grown = realloc(themes->ids, (capacity + found) * sizeof *themes->ids);
        if (!grown) {
            snx_free_strings(names, found);
            continue;
        }
        themes->ids = grown;
        capacity += found;
        for (size_t i = 0; i < found; ++i) {
            char* folder = snx_path_join(dirs[d], names[i]);
            char* file = folder ? snx_path_join(folder, "theme.ini") : NULL;
            bool usable = names[i][0] != '.' && !listed(themes, names[i]) &&
                          strlen(names[i]) < sizeof(((GuiTheme*)0)->id) && file && snx_is_file(file);
            free(file);
            free(folder);
            if (usable) {
                themes->ids[themes->count++] = names[i];
                names[i] = NULL;
            }
        }
        snx_free_strings(names, found);
    }
}
