/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_game_ini(const char* folder, const char* name) {
    char *own = snx_user_path(folder, SNX_USER_CONFIG), *base = snx_path_join(own, name);
    char* path = snx_format("%s.ini", base);
    free(own);
    free(base);
    return path;
}
/* key's value in an .ini's text, the last one's, as the runtime reads it: in the text, NULL for none */
static char* text_value(char* text, const char* key) {
    SnxIni ini;
    char *k, *value, *found = NULL;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &k, &value))
        if (!strcmp(k, key)) found = value;
    return found;
}
char* snx_ini_value(const char* path, const char* key) {
    size_t size;
    char *text = (char*)snx_read_file(path, &size), *value = text ? text_value(text, key) : NULL;
    char* copy = value && *value ? snx_strdup(value) : NULL;
    free(text);
    return copy;
}
/* a line, from line to end, that sets key= */
static int key_line(const char* line, const char* end, const char* key) {
    size_t n = strlen(key);
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if ((size_t)(end - line) < n || strncmp(line, key, n)) return 0;
    for (line += n; line < end && (*line == ' ' || *line == '\t'); line++) continue;
    return line < end && *line == '=';
}
/* a line, from line to end, that starts [Game] */
static int game_line(const char* line, const char* end) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    return end - line == 6 && !strncmp(line, "[Game]", 6);
}
int snx_ini_record(const char* path, const char* key, const char* value) {
    char *dir = snx_dirname(path), *tmp = snx_format("%s.tmp", path), *out = NULL, *text, *copy, *had;
    const char *line, *end, *next;
    size_t size = 0, n = 0, len = strlen(key) + strlen(value);
    int placed = 0, rc = 0;
    if (value[strcspn(value, "\r\n")]) {
        rc = snx_error("%s=%s has a line break, which the game's settings cannot keep", key, value);
        goto done;
    }
    text = (char*)snx_read_file(path, &size);
    if (!text && snx_path_exists(path)) {
        rc = snx_error("cannot read %s to record %s= in it", path, key);
        goto done;
    }
    if (text) {
        copy = snx_strdup(text);
        had = text_value(copy, key);
        placed = had && !strcmp(had, value);
        free(copy);
        if (placed) {
            free(text);
            goto done;
        }
    }
    out = (char*)snx_alloc(size + len + 24);
    for (line = text; line && line < text + size; line = next) {
        end = (const char*)memchr(line, '\n', (size_t)(text + size - line));
        next = end ? end + 1 : text + size;
        if (!end) end = next;
        if (key_line(line, end, key)) continue;
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
        if (next == end) out[n++] = '\n'; /* the file's last line, without a newline */
        if (!placed && game_line(line, end)) {
            n += (size_t)sprintf(out + n, "%s=%s\n", key, value);
            placed = 1;
        }
    }
    free(text);
    if (!placed) n += (size_t)sprintf(out + n, "%s[Game]\n%s=%s\n", n ? "\n" : "", key, value);
    if (snx_mkdirs(dir) || snx_write_file(tmp, out, n) || snx_replace(tmp, path)) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot record %s=%s in %s", key, value, path);
    }
done:
    free(dir);
    free(tmp);
    free(out);
    return rc;
}
size_t snx_addons_parse(const char* text, const char* game, SnxU64* ids, size_t max) {
    char word[17];
    size_t count = 0, n, i;
    SnxU64 own, id;
    if (!snx_title_parse(game, &own)) return 0;
    while (text && *text) {
        while (*text == ',' || *text == ' ' || *text == '\t') text++;
        for (n = 0; text[n] && text[n] != ',' && text[n] != ' ' && text[n] != '\t'; n++) continue;
        if (n == 16) {
            for (i = 0; i < 16; i++) word[i] = (char)toupper((unsigned char)text[i]);
            word[16] = 0;
            if (snx_title_parse(word, &id) && snx_addon_kind(own, id) != SNX_ADDON_NONE) {
                for (i = 0; i < count && ids[i] != id; i++) continue;
                if (i == count && count < max) ids[count++] = id;
            }
        }
        text += n;
    }
    return count;
}
char* snx_addons_text(const SnxU64* ids, size_t count) {
    char* text = (char*)snx_alloc(count * 17 + 1);
    size_t i;
    text[0] = 0;
    for (i = 0; i < count; i++) {
        if (i) strcat(text, ",");
        snx_title_text(ids[i], text + strlen(text));
    }
    return text;
}
