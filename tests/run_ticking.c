/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_run_ticking (src/common/utilities/common.c): the tick comes about once a second while the child runs, its
   output captured or not, with what the child has written so far, and the child's output and status are what snx_run
   gives */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "run_ticking.c:%d: check failed: %s\n", __LINE__, #e);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static void pause_ms(long ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = ms % 1000 * 1000000L;
    while (nanosleep(&t, &t)) {}
#endif
}
typedef struct {
    int ticks;
    SnxU64 written; /* the most a tick was told, SNX_RUN_UNCOUNTED while none counted */
} Seen;
static void count(void* data, SnxU64 written) {
    Seen* seen = (Seen*)data;
    ++seen->ticks;
    if (written != SNX_RUN_UNCOUNTED && (seen->written == SNX_RUN_UNCOUNTED || written > seen->written))
        seen->written = written;
}
static void seen_reset(Seen* seen) {
    seen->ticks = 0;
    seen->written = SNX_RUN_UNCOUNTED;
}
#define WRITTEN 65536 /* what the child writes to its file before it waits */

int main(int argc, char** argv) {
    const char *command[4], *quick[4];
    char *self, *output, *file_path;
    Seen seen;
    int rc;
    double start, took;
    if (argc >= 3 && !strcmp(argv[1], "--child")) { /* a tool that says something, works a while, and fails */
        puts("before");
        fflush(stdout);
        if (strcmp(argv[2], "now")) { /* "--child FILE" writes to FILE, then waits for a tick; "--child now" doesn't */
            static char block[WRITTEN];
            FILE* file = fopen(argv[2], "wb");
            if (!file || fwrite(block, 1, sizeof block, file) != sizeof block || fclose(file)) return 4;
            pause_ms(1600);
        }
        puts("after");
        return 3;
    }
    self = snx_executable(argv[0]);
    CHECK(self != NULL);
    file_path = (char*)malloc(strlen(self) + 9);
    CHECK(file_path != NULL);
    strcpy(file_path, self);
    strcat(file_path, ".written");
    command[0] = self;
    command[1] = "--child";
    command[2] = file_path;
    command[3] = NULL;
    /* captured: the output whole, the status, a tick a second of the child's time, which knows what it wrote */
    seen_reset(&seen);
    output = NULL;
    start = snx_seconds();
    rc = snx_run_ticking(command, NULL, NULL, &output, count, &seen);
    took = snx_seconds() - start;
    CHECK(rc == 3 && output && strstr(output, "before") && strstr(output, "after"));
    CHECK(seen.ticks >= 1 && seen.ticks <= (int)took);
#if defined(__linux__) || defined(_WIN32) /* the systems that count what a process writes, its files included */
    CHECK(seen.written != SNX_RUN_UNCOUNTED && seen.written >= WRITTEN);
#endif
    free(output);
    /* not captured: the child's end is waited for, ticking too */
    seen_reset(&seen);
    rc = snx_run_ticking(command, NULL, NULL, NULL, count, &seen);
    CHECK(rc == 3 && seen.ticks >= 1);
#if defined(__linux__) || defined(_WIN32)
    CHECK(seen.written != SNX_RUN_UNCOUNTED && seen.written >= WRITTEN);
#endif
    remove(file_path);
    free(file_path);
    /* snx_run, no tick */
    quick[0] = self;
    quick[1] = "--child";
    quick[2] = "now";
    quick[3] = NULL;
    output = NULL;
    CHECK(snx_run(quick, NULL, NULL, &output) == 3 && output && strstr(output, "after"));
    free(output);
    free(self);
    puts("run_ticking passed");
    return 0;
}
