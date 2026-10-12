/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "driver.h"
#include "common.h"
#include "extract.h"
#include "recompile.h"
#include <stdlib.h>
#include <string.h>
#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR "build"
#endif

static void usage(void) {
    puts("Usage: scarletnx [--repo DIR] [--build-dir DIR] COMMAND [OPTIONS]\n"
         "  --extract DUMP [--update DUMP] [--name NAME]  Extract XCI/NSP\n"
         "  --recompile INPUT [NAME] [OPTIONS]          Recompile ExeFS/NRO\n"
         "  --extract --help                           Extraction options\n"
         "  --recompile --help                         Recompiler options\n"
         "  --game=games/Game                          Run a compiled game\n"
         "Tool paths default to this build's checkout and build directory.");
}
int snx_driver(int argc, char** argv) {
    SnxPaths paths;
    const char *repo = SNX_SOURCE_DIR, *build = SNX_BUILD_DIR;
    char *r, *b, *exe;
    int i = 1, mode = 0, result;
    if (argc <= 1) return -1;
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        snx_tool_console();
        usage();
        return 0;
    }
    while (i < argc) {
        if (!strcmp(argv[i], "--repo") || !strcmp(argv[i], "--build-dir")) {
            int is_repo = !strcmp(argv[i], "--repo");
            if (++i == argc) return snx_error("%s requires a directory", argv[i - 1]);
            if (is_repo)
                repo = argv[i];
            else
                build = argv[i];
            ++i;
        } else
            break;
    }
    if (i < argc && !strcmp(argv[i], "--extract")) mode = 1;
    if (i < argc && !strcmp(argv[i], "--recompile")) mode = 2;
    if (!mode) {
        if (i > 1) return snx_error("expected --extract or --recompile");
        return -1;
    }
    snx_tool_console();
    r = snx_absolute(repo);
    b = snx_absolute(build);
    exe = snx_executable(argv[0]);
    if (!r || !b || !exe) {
        free(r);
        free(b);
        free(exe);
        return snx_error("cannot resolve tool paths");
    }
    paths.repo = r;
    paths.build = b;
    paths.executable = exe;
    result =
        mode == 1 ? snx_extract(argc - i - 1, argv + i + 1, &paths) : snx_recompile(argc - i - 1, argv + i + 1, &paths);
    free(r);
    free(b);
    free(exe);
    return result;
}
