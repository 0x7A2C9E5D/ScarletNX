/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
#include "common.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void usage(FILE* out) {
    fputs("Usage: scarletnx-3ds [--model old|new] [--] game.cia\n"
          "       scarletnx-3ds [--model old|new] --board-info\n"
          "Stage 1: board/launcher foundation only; CIA extraction and execution are not implemented.\n", out);
}
static bool cia_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'c' &&
           tolower((unsigned char)path[n - 2]) == 'i' && tolower((unsigned char)path[n - 1]) == 'a';
}
int main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char* input = NULL;
    bool info = false, positional = false;
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--")) positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info")) info = true;
        else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else input = arg;
    }
    if ((!input && !info) || (input && info)) {
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) return 1;
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %" PRIu64 " Hz\nFCRAM: %u MiB\n",
               board.profile->name, board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ,
               board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    if (!cia_path(input) || !snx_is_file(input)) {
        fputs("scarletnx-3ds: expected a readable .cia file\n", stderr);
        return 2;
    }
    FILE* file = snx_fopen(input, "rb");
    if (!file) {
        fputs("scarletnx-3ds: cannot read CIA input\n", stderr);
        return 2;
    }
    fclose(file);
    /* Never route CTR content to NX's A64 compiler, or emit a launch/success banner. */
    fputs("scarletnx-3ds: CIA extraction, ARM11 AOT compilation and execution are not implemented (stage 2).\n",
          stderr);
    return 3;
}
