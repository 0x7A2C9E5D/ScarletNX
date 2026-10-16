/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the .ini's settings tables (ini.h): finding a key, reading a value into its variable, and writing it back */
#include "runtime/ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const IniOpt* ini_find(const IniOpt* table, const char* key) {
    for (; table->key; ++table)
        if (!strcmp(table->key, key)) return table;
    return NULL;
}

void ini_read(const IniOpt* o, const char* v) {
    char* end;
    double d;
    if (o->type == INI_TEXT) {
        o->set(o, v);
        return;
    }
    d = strtod(v, &end);
    if (end == v || d != d) return; /* NaN would pass the range unclamped */
    if (d < o->lo) d = o->lo;
    if (d > o->hi) d = o->hi;
    /* atomic: other threads read some of the settings while a reload writes them (the GPU thread's vsync) */
    if (o->type == INI_INT)
        __atomic_store_n((int*)o->var, (int)d, __ATOMIC_RELAXED);
    else {
        float f = (float)d;
        __atomic_store((float*)o->var, &f, __ATOMIC_RELAXED);
    }
    if (o->set) o->set(o, v);
}

void ini_format(const IniOpt* o, char* out, size_t n) {
    if (o->type == INI_INT)
        snprintf(out, n, "%d", *(const int*)o->var);
    else if (o->type == INI_FLOAT)
        snprintf(out, n, "%g", *(const float*)o->var);
    else if (n)
        out[0] = 0;
}

static int step_of(const IniOpt* o) { return o->step > 0 ? o->step : 1; }
void ini_word(const IniOpt* o, int v, char* out, size_t n) {
    const char* w = o->words;
    double at = o->lo;
    size_t len;
    for (; w; w += len + 1, at += step_of(o)) {
        const char* mark;
        len = strcspn(w, "|");
        mark = (const char*)memchr(w, '#', len);
        if (mark) { /* a number from here on */
            snprintf(out, n, "%.*s%d%.*s", (int)(mark - w), w, v + o->shift, (int)(len - (size_t)(mark - w) - 1),
                     mark + 1);
            return;
        }
        if ((double)v == at) {
            snprintf(out, n, "%.*s", (int)len, w);
            return;
        }
        if (!w[len]) break;
    }
    snprintf(out, n, "%d", v); /* no word for it */
}
int ini_step(const IniOpt* o, int v, int dir) {
    int step = step_of(o), count;
    if (!o->words || strchr(o->words, '#')) {
        double n = (double)v + (double)dir * step;
        return n < o->lo ? (int)o->lo : n > o->hi ? (int)o->hi : (int)n;
    }
    count = (int)((o->hi - o->lo) / step) + 1;
    return (int)o->lo + (((v - (int)o->lo) / step + dir) % count + count) % count * step;
}
