/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_progress: the lines a program following a run reads from SNX_PROGRESS_FILE. One JSON object each, appended whole;
   nothing, and no error, without the variable or when the file cannot be written */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define SET_ENV(name, value) _putenv_s(name, value)
#else
#define SET_ENV(name, value) setenv(name, value, 1)
#endif

int main(void) {
    char *dir = snx_temp_dir(NULL), *file, *text;
    size_t size;
    unsigned char* data;
    assert(dir);
    file = snx_path_join(dir, "progress.jsonl");

    SET_ENV("SNX_PROGRESS_FILE", "");
    snx_progress("extract", -1, -1, NULL); /* the variable empty: nothing */
    assert(!snx_is_file(file));

    SET_ENV("SNX_PROGRESS_FILE", file);
    snx_progress("extract", -1, -1, NULL);
    snx_progress("recompile", -1, -1, "A \"Game\"\\Two");
    snx_progress("block", 3, 6, NULL);
    snx_progress("launched", -1, -1, "Caf\xc3\xa9\n"); /* UTF-8 kept, the newline escaped */
    data = snx_read_file(file, &size);
    assert(data);
    text = (char*)snx_alloc(size + 1);
    memcpy(text, data, size);
    text[size] = 0;
    free(data);
    assert(!strcmp(text, "{\"v\":1,\"event\":\"extract\"}\n"
                         "{\"v\":1,\"event\":\"recompile\",\"title\":\"A \\\"Game\\\"\\\\Two\"}\n"
                         "{\"v\":1,\"event\":\"block\",\"done\":3,\"total\":6}\n"
                         "{\"v\":1,\"event\":\"launched\",\"title\":\"Caf\xc3\xa9\\u000a\"}\n"));
    free(text);

    { /* a title past the line's room is cut, not overrun, and the file stays one object per line */
        char* big = (char*)snx_alloc(4000);
        memset(big, 'x', 3999);
        big[3999] = 0;
        assert(!snx_remove_tree(file));
        snx_progress("built", -1, -1, big);
        data = snx_read_file(file, &size);
        assert(data && size > 0 && size < 1024 && data[size - 1] == '\n' && data[size - 2] == '}');
        free(data);
        free(big);
    }

    SET_ENV("SNX_PROGRESS_FILE", dir); /* a folder: cannot be opened, and that must not stop the run */
    snx_progress("block", 1, 2, NULL);
    SET_ENV("SNX_PROGRESS_FILE", "");
    assert(!snx_remove_tree(dir));
    free(file);
    free(dir);
    puts("progress: ok");
    return 0;
}
