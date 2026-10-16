/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "settings.h"
#include "common.h"
#include "ini_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* const off_on[] = {"off", "on", NULL};
static const char* const off[] = {"off", NULL}; /* libgccjit in a build without it */

static void row(GuiSettings* s, const char* section, const char* key, const char* about, const char* const* words) {
    GuiThemeKey* k = &s->keys[s->count];
    GuiSetting* v = &s->values[s->count++];
    k->section = section;
    k->key = key;
    k->about = about;
    k->words = words;
    k->type = words ? GUI_KEY_WORD : GUI_KEY_TEXT;
    k->at = words ? (size_t)((char*)&v->word - (char*)s->values) : (size_t)((char*)v->text - (char*)s->values);
    k->size = words ? sizeof v->word : sizeof v->text;
    k->max = 0;
}
int gui_settings_read(GuiSettings* s, const char* list) {
    size_t size = strlen(list) + 1, n = GUI_SETTINGS_OWN;
    const char* c;
    char *line, *next, *text;
    gui_settings_free(s);
    for (c = list; *c; ++c) n += *c == '\n'; /* a row a line at most */
    if (n > ((size_t)-1 - size) / (sizeof *s->keys + sizeof *s->values)) return 1;
    s->keys = malloc(n * (sizeof *s->keys + sizeof *s->values) + size);
    if (!s->keys) return 1;
    s->values = (GuiSetting*)(s->keys + n);
    text = memcpy(s->values + n, list, size);
    memset(s->values, 0, n * sizeof *s->values);
    row(s, "Launch", "libgccjit",
        SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm" : "Program was not compiled with libgccjit",
        SNX_GUI_HAS_GCCJIT ? off_on : off);
    row(s, "Launch", "gdbstub", "Enables gdbstub on port 1234", off_on);
    for (line = text; *line; line = next) {
        char* field[7];
        int f = 1;
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        field[0] = line;
        while (f < 7 && (field[f] = strchr(field[f - 1], '\t')) != NULL) *field[f++]++ = 0;
        if (f < 7) continue; /* not a setting: another line the runtime printed */
        {
            GuiSetting* v = &s->values[s->count];
            double lo = strtod(field[4], NULL), hi = strtod(field[5], NULL);
            int i, words = !strcmp(field[3], "int") && lo >= 0 && hi - lo < GUI_SETTING_WORDS ? (int)(hi - lo) + 1 : 0;
            v->words[0] = ".ini";
            for (i = 0; i < words; ++i) {
                snprintf(v->numbers[i], sizeof v->numbers[i], "%d", (int)lo + i);
                v->words[i + 1] = v->numbers[i];
            }
            row(s, field[0], field[1], field[6], words ? v->words : NULL);
        }
    }
    return 0;
}
void gui_settings_free(GuiSettings* s) {
    free(s->keys);
    memset(s, 0, sizeof *s);
}
void gui_settings_mark(GuiSettings* s) {
    int i;
    for (i = 0; i < s->count; ++i) gui_theme_get(s->values, &s->keys[i], s->values[i].start, sizeof s->values[i].start);
}
void gui_settings_load(GuiSettings* s, const char* path) {
    size_t size;
    char *text = path ? (char*)snx_read_file(path, &size) : NULL, *key, *value;
    SnxIni ini;
    if (!text) return;
    snx_ini_start(&ini, text); /* a key names one row whatever its section, as in the game's .ini */
    while (snx_ini_next(&ini, &key, &value)) gui_settings_set(s, key, value);
    free(text);
}
int gui_settings_save(const GuiSettings* s, const char* path) {
    size_t room = 256 + (size_t)s->count * 200, at = 0; /* a row's section, key and value take less than 200 */
    char *text = (char*)malloc(room), *folder = snx_dirname(path), *staged = snx_format("%s.new", path), value[64];
    const char* section = "";
    int i, failed;
    if (text)
        at = (size_t)snprintf(text, room,
                              "; what the launcher's Settings changed, for every game it runs: over each "
                              "game's .ini\n");
    for (i = 0; text && i < s->count; ++i) {
        gui_theme_get(s->values, &s->keys[i], value, sizeof value);
        if (!strcmp(value, s->values[i].start)) continue;
        if (strcmp(section, s->keys[i].section))
            at += (size_t)snprintf(text + at, room - at, "[%s]\n", section = s->keys[i].section);
        at += (size_t)snprintf(text + at, room - at, "%s=%s\n", s->keys[i].key, value);
    }
    /* staged beside it, then put in place: a crash mid-write leaves the old picks */
    failed = !text || snx_mkdirs(folder) || snx_write_file(staged, text, at) || snx_replace(staged, path);
    if (failed) remove(staged);
    free(staged);
    free(folder);
    free(text);
    return failed;
}
static int find(const GuiSettings* s, const char* key) {
    int i;
    for (i = 0; i < s->count && strcmp(s->keys[i].key, key); ++i) {}
    return i;
}
/* row i's pick, NULL for the .ini's (the launcher's: off) */
static const char* picked(const GuiSettings* s, int i) {
    const GuiSetting* v = &s->values[i];
    if (s->keys[i].type == GUI_KEY_WORD) return v->word ? s->keys[i].words[v->word] : NULL;
    return v->text[0] ? v->text : NULL;
}
int gui_settings_set(GuiSettings* s, const char* key, const char* value) {
    int i = find(s, key);
    return i < s->count && gui_theme_set(s->values, &s->keys[i], value);
}
const char* gui_settings_value(const GuiSettings* s, const char* key) {
    int i = find(s, key);
    return i < s->count ? picked(s, i) : NULL;
}
size_t gui_settings_options(GuiSettings* s, int launch, int prepared, const char** argv) {
    size_t n = 0;
    int i;
    for (i = GUI_SETTINGS_OWN; i < s->count; ++i) {
        const char* value = picked(s, i);
        if (!value) continue;
        snprintf(s->values[i].option, sizeof s->values[i].option, "--%s=%s", s->keys[i].key, value);
        argv[n++] = s->values[i].option;
    }
    if (launch && s->count >= GUI_SETTINGS_OWN && s->values[0].word && !prepared) argv[n++] = "--backend=gcc";
    if (launch && s->count >= GUI_SETTINGS_OWN && s->values[1].word) {
        argv[n++] = "-S";
        argv[n++] = "-s";
    }
    return n;
}
