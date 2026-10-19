/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/driver/game_name.h"
#include "common/utilities/ini_lines.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* snx_game_title(const char* name, const SnxNpdm* npdm, const char* input) {
    const char *start, *s;
    char *out, *dot;
    size_t n = 0;
    int input_name = 0;
    if (!name && npdm->name[0] && strcmp(npdm->name, "Application")) name = npdm->name;
    if (!name) {
        start = input;
        for (s = input; *s; ++s)
            if (*s == '/' || *s == '\\') start = s + 1;
        name = start;
        input_name = 1;
    }
    out = (char*)snx_alloc(strlen(name) + 1);
    /* Preserve UTF-8 names; exclude path delimiters and C/OS metacharacters. */
    for (s = name; *s; ++s)
        if ((unsigned char)*s >= 128 || isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == ' ' ||
            (input_name && *s == '.'))
            out[n++] = *s;
    out[n] = 0;
    if (input_name && (dot = strrchr(out, '.')) != NULL) *dot = 0;
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) {
        free(out);
        return snx_strdup("Game");
    }
    return out;
}
char* snx_game_shown(const char* name, const SnxProgram* program) {
    char shown[0x200];
    if (!name && snx_nacp_name(program->nacp, program->nacp_size, shown, sizeof shown)) return snx_strdup(shown);
    return snx_game_title(name, &program->npdm, program->entry_path);
}
char* snx_game_name(const char* name, const SnxNpdm* npdm, const char* input) {
    char id[17];
    if (name || !snx_title_text(npdm->title_id, id)) return snx_game_title(name, npdm, input);
    return snx_strdup(id);
}
char* snx_game_ini(const char* folder, const char* name) {
    char *own = snx_user_path(folder, SNX_USER_CONFIG), *base = snx_path_join(own, name);
    char* path = snx_format("%s.ini", base);
    free(own);
    free(base);
    return path;
}
/* source='s value in an .ini's text, the last one's, as the runtime reads it: in the text, NULL for none */
static char* ini_source(char* text) {
    SnxIni ini;
    char *key, *value, *source = NULL;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value))
        if (!strcmp(key, "source")) source = value;
    return source;
}
char* snx_game_source(const char* folder, const char* name, int* recorded) {
    char *ini = snx_game_ini(folder, name), *path;
    size_t size;
    char* text = (char*)snx_read_file(ini, &size);
    char* source = text ? ini_source(text) : NULL;
    int set = source && *source;
    path = set ? snx_strdup(source) : snx_format("%s/%s.source", folder, name);
    if (recorded) *recorded = set || snx_is_link(path) || snx_path_exists(path);
    free(text);
    free(ini);
    return path;
}
/* a line, from line to end, that sets source=, which the rewrite leaves out */
static int source_line(const char* line, const char* end) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (end - line < 6 || strncmp(line, "source", 6)) return 0;
    for (line += 6; line < end && (*line == ' ' || *line == '\t'); line++) continue;
    return line < end && *line == '=';
}
/* a line, from line to end, that starts [Game] */
static int game_line(const char* line, const char* end) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    return end - line == 6 && !strncmp(line, "[Game]", 6);
}
int snx_record_source(const char* folder, const char* name, const char* dump) {
    char *ini = snx_game_ini(folder, name), *dir = snx_dirname(ini), *tmp = snx_format("%s.tmp", ini), *out = NULL;
    char *text, *copy, *source;
    const char *line, *end, *next;
    size_t size = 0, n = 0, len = strlen(dump);
    int placed = 0, rc = 0;
    if (dump[strcspn(dump, "\r\n")]) {
        rc = snx_error("the dump's path %s has a line break, which the game's settings cannot keep", dump);
        goto done;
    }
    text = (char*)snx_read_file(ini, &size);
    if (!text && snx_path_exists(ini)) {
        rc = snx_error("cannot read %s to record the game's dump in it", ini);
        goto done;
    }
    if (text) {
        copy = snx_strdup(text);
        source = ini_source(copy);
        placed = source && !strcmp(source, dump);
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
        if (source_line(line, end)) continue;
        memcpy(out + n, line, (size_t)(next - line));
        n += (size_t)(next - line);
        if (next == end) out[n++] = '\n'; /* the file's last line, without a newline */
        if (!placed && game_line(line, end)) {
            n += (size_t)sprintf(out + n, "source=%s\n", dump);
            placed = 1;
        }
    }
    free(text);
    if (!placed) n += (size_t)sprintf(out + n, "%s[Game]\nsource=%s\n", n ? "\n" : "", dump);
    if (snx_mkdirs(dir) || snx_write_file(tmp, out, n) || snx_replace(tmp, ini)) {
        snx_remove_tree(tmp);
        rc = snx_error("cannot record the game's dump %s in %s", dump, ini);
    }
done:
    free(ini);
    free(dir);
    free(tmp);
    free(out);
    return rc;
}
