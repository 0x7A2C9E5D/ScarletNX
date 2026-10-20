/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/board.h"
#include "3ds/loader/ctr_loader.h"
#include "3ds/aot/ctr_module.h"
#include "common/utilities/common.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR NULL
#endif

static void usage(FILE* out) {
    fputs("Usage: scarletnx-3ds [OPTIONS] game.cia\n"
          "       scarletnx-3ds [--model old|new] --board-info\n"
          "  game.cia        Read a decrypted application CIA where it lies, recompile if needed, then play\n"
          "  --extract-container\n"
          "                  Extract the CIA into its folder first and play from the extraction\n"
          "  -x, --extract   Extract only\n"
          "  -r, --recomp    Recompile without playing\n"
          "  --model old|new The console to model (default old)\n"
          "  --repo DIR      The data folder (default: a portable copy's folder, else the user's data\n"
          "                  folder, as scarletnx picks it; the user's keeps work/ in its cache folder)\n"
          "  --output DIR    The title's folder: its compiled module, SD card and any extraction (default\n"
          "                  work/3ds/<the CIA's name>)\n"
          "  --max-blocks N  Stop after N native blocks (default: run until the game exits or its window\n"
          "                  closes)\n"
          "Without --extract-container nothing of the CIA is extracted: its ExeFS and RomFS are read from the\n"
          "CIA itself, mapped read-only. An extraction and a compiled module already there are used as they\n"
          "are: the ARM11 code is compiled again only when its module is missing or no longer matches the\n"
          "build or the code. Encrypted content is not implemented.\n",
          out);
}
static bool cia_path(const char* path) {
    size_t n = strlen(path);
    return n >= 4 && path[n - 4] == '.' && tolower((unsigned char)path[n - 3]) == 'c' &&
           tolower((unsigned char)path[n - 2]) == 'i' && tolower((unsigned char)path[n - 1]) == 'a';
}
static bool visible_path(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    return true;
}
/* work/3ds of the data folder the Switch driver works in (snx_data_dir), which the user's keeps in its cache folder
   (snx_work_dir); resolved, as the loader refuses a link on the way to its output */
static char* work_folder(const char* repo, const char* argv0) {
    char *exe = repo ? NULL : snx_executable(argv0), *dir = exe ? snx_dirname(exe) : NULL;
    char* data = repo ? snx_strdup(repo) : snx_data_dir(dir);
    char* checkout = repo ? NULL : snx_checkout(SNX_SOURCE_DIR, SNX_BUILD_DIR, dir);
    char *work = data ? snx_work_dir(data) : snx_strdup("work"), *folder = snx_path_join(work, "3ds");
    char* resolved = snx_absolute(folder);
    if (!resolved) {
        char why[256];
        fprintf(stderr, "scarletnx-3ds: cannot resolve %s: %s\n", folder, snx_os_error(why, sizeof why));
    }
    if (checkout && data) snx_checkout_left(checkout, data);
    free(checkout);
    free(folder);
    free(work);
    free(data);
    free(dir);
    free(exe);
    return resolved;
}
/* what an extraction writes into its folder, beside which the title's state (native/, sdmc/, frames/, audio.wav) is
   kept */
static const char* const extraction_parts[] = {"exefs",          "romfs",     "contents", "cia-header.bin",
                                               "certificates.bin", "ticket.bin", "tmd.bin", "metadata.bin",
                                               "ncch.bin",       "exheader.bin", "manifest.ini"};
static bool holds_extraction(const char* folder) {
    char* exheader = snx_path_join(folder, "exheader.bin");
    bool found = snx_is_file(exheader);
    free(exheader);
    return found;
}
/* The CIA extracted into folder: published there, or, when the folder is there already with the title's state but no
   extraction (the CIA was read where it lies before), extracted beside it and moved into it */
static SnxCtrExtractResult extract_into(const char* input, const char* folder, char** published, char* error,
                                        size_t error_size) {
    /* anything but a folder of its own (nothing, a link, a file) is the extractor's to take or refuse */
    if (snx_is_link(folder) || !snx_is_dir(folder))
        return snx_ctr_extract(input, folder, NULL, published, error, error_size);
    char* staging = snx_format("%s.extracting", folder);
    char *done = NULL, **names = NULL;
    size_t count = 0;
    SnxCtrExtractResult result = SNX_CTR_EXTRACT_IO;
    char why[256];
    if (snx_path_exists(staging) && snx_remove_tree(staging)) {
        snprintf(error, error_size, "cannot remove %s: %s", staging, snx_os_error(why, sizeof why));
        goto out;
    }
    result = snx_ctr_extract(input, staging, NULL, &done, error, error_size);
    if (result != SNX_CTR_EXTRACT_OK) goto out;
    result = SNX_CTR_EXTRACT_IO;
    if (snx_list_dir(done, &names, &count)) {
        snprintf(error, error_size, "cannot list %s: %s", done, snx_os_error(why, sizeof why));
        goto out;
    }
    for (size_t i = 0; i < count; ++i) {
        char *from = snx_path_join(done, names[i]), *to = snx_path_join(folder, names[i]);
        /* only a stopped extraction's part can be in the way: the state has other names */
        bool moved = !(snx_path_exists(to) && snx_remove_tree(to)) && !snx_replace(from, to);
        if (!moved) snprintf(error, error_size, "cannot move %s to %s: %s", from, to, snx_os_error(why, sizeof why));
        free(from);
        free(to);
        if (!moved) goto out;
    }
    *published = snx_absolute(folder);
    if (*published)
        result = SNX_CTR_EXTRACT_OK;
    else
        snprintf(error, error_size, "cannot resolve %s: %s", folder, snx_os_error(why, sizeof why));
out:
    if (done && snx_path_exists(done)) snx_remove_tree(done);
    snx_free_strings(names, count);
    free(done);
    free(staging);
    return result;
}
/* The default: nothing extracted. The CIA is mapped and checked as the extraction checks it, its ExeFS and RomFS read
   where they lie, and folder keeps the title's state alone: an extraction an earlier --extract-container left there
   is taken away, as the CIA has all of it */
static int play_cia(const char* input, const char* folder, SnxCtrModel model, uint64_t max_blocks, unsigned flags,
                    char* error, size_t error_size) {
    SnxCtrCia* cia = NULL;
    printf("Reading %s\n", input);
    SnxCtrExtractResult opened = snx_ctr_cia_open(input, &cia, error, error_size);
    if (opened != SNX_CTR_EXTRACT_OK) {
        fprintf(stderr, "scarletnx-3ds: %s\n", error);
        return opened == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
    }
    char *state = NULL, why[256];
    int status = 1;
    if (snx_is_link(folder))
        snprintf(why, sizeof why, "it is a link");
    else if (snx_mkdirs(folder) || !(state = snx_absolute(folder)))
        snx_os_error(why, sizeof why);
    if (!state) {
        fprintf(stderr, "scarletnx-3ds: cannot create the title's folder %s: %s\n", folder, why);
        goto done;
    }
    printf("The title's folder is %s\n", state);
    if (holds_extraction(state)) {
        printf("Removing the extraction in %s, the game reads its CIA\n", state);
        for (size_t i = 0; i < sizeof extraction_parts / sizeof *extraction_parts; ++i) {
            char* part = snx_path_join(state, extraction_parts[i]);
            bool failed = snx_path_exists(part) && snx_remove_tree(part);
            if (failed) fprintf(stderr, "scarletnx-3ds: cannot remove %s: %s\n", part, snx_os_error(why, sizeof why));
            free(part);
            if (failed) goto done;
        }
    }
    status = snx_ctr_aot_run(state, cia, model, max_blocks, flags, error, error_size);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
done:
    free(state);
    snx_ctr_cia_close(cia);
    return status;
}
int main(int argc, char** argv) {
    SnxCtrModel model = SNX_CTR_OLD;
    const char *input = NULL, *output = NULL, *repo = NULL;
    bool info = false, positional = false, extract_only = false, recompile = false, budget_set = false;
    bool extract_container = false;
    uint64_t max_blocks = 0; /* no budget */
    SnxCtrBoard board;
    snx_tool_console();
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!positional && !strcmp(arg, "--"))
            positional = true;
        else if (!positional && (!strcmp(arg, "--help") || !strcmp(arg, "-h"))) {
            usage(stdout);
            return 0;
        } else if (!positional && !strcmp(arg, "--board-info"))
            info = true;
        else if (!positional && (!strcmp(arg, "-x") || !strcmp(arg, "--extract")))
            extract_only = true;
        else if (!positional && !strcmp(arg, "--extract-container"))
            extract_container = true;
        else if (!positional && (!strcmp(arg, "-r") || !strcmp(arg, "--recomp")))
            recompile = true;
        else if (!positional && !strcmp(arg, "--output")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --output requires a directory\n", stderr);
                return 2;
            }
            output = argv[i];
        } else if (!positional && !strcmp(arg, "--repo")) {
            if (++i == argc || !*argv[i]) {
                fputs("scarletnx-3ds: --repo requires a directory\n", stderr);
                return 2;
            }
            repo = argv[i];
        } else if (!positional && !strcmp(arg, "--max-blocks")) {
            char* end = NULL;
            if (++i == argc || argv[i][0] < '0' || argv[i][0] > '9') {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                return 2;
            }
            errno = 0;
            unsigned long long n = strtoull(argv[i], &end, 10);
            if (errno || *end || !n) {
                fputs("scarletnx-3ds: --max-blocks requires a positive number\n", stderr);
                return 2;
            }
            max_blocks = (uint64_t)n;
            budget_set = true;
        } else if (!positional && !strcmp(arg, "--model")) {
            if (++i == argc || (strcmp(argv[i], "old") && strcmp(argv[i], "new"))) {
                fputs("scarletnx-3ds: --model requires old or new\n", stderr);
                return 2;
            }
            model = !strcmp(argv[i], "old") ? SNX_CTR_OLD : SNX_CTR_NEW;
        } else if ((!positional && arg[0] == '-') || input) {
            fprintf(stderr, "scarletnx-3ds: unexpected argument: %s\n", arg);
            usage(stderr);
            return 2;
        } else
            input = arg;
    }
    if ((!input && !info) ||
        (info && (input || extract_only || extract_container || recompile || output || budget_set)) ||
        (extract_only && (budget_set || recompile)) || (recompile && budget_set)) {
        usage(stderr);
        return 2;
    }
    if (!snx_ctr_board_init(&board, model)) return 1;
    if (info) {
        printf("3DS board: %s\nARM11 cores: %u\nBase clock: %" PRIu64 " Hz\nFCRAM: %u MiB\n", board.profile->name,
               board.profile->arm11_cores, SNX_CTR_BASE_CLOCK_HZ, board.profile->fcram_size / (1024u * 1024u));
        for (size_t i = 0; i < board.profile->region_count; ++i) {
            const SnxCtrRegion* r = &board.profile->regions[i];
            printf("  %-10s physical 0x%08" PRIx32 " size 0x%08" PRIx32 "\n", r->name, r->base, r->size);
        }
        snx_ctr_board_shutdown(&board);
        return 0;
    }
    snx_ctr_board_shutdown(&board);
    /* Do not let a host filename inject progress/launch banners into the GUI. */
    if (!visible_path(input) || (output && !visible_path(output))) {
        fputs("scarletnx-3ds: input/output paths may not contain control characters\n", stderr);
        return 2;
    }
    const char* refused = !cia_path(input)          ? "is not a .cia file"
                          : !snx_path_exists(input) ? "does not exist"
                          : !snx_is_file(input)     ? "is not a file"
                                                    : NULL;
    if (refused) {
        fprintf(stderr, "scarletnx-3ds: %s %s\n", input, refused);
        return 2;
    }
    char error[512], *extracted = NULL;
    if (output)
        extracted = snx_strdup(output); /* as given: the extractor refuses links on the way to it */
    else {
        char *work = work_folder(repo, argv[0]), *name = snx_dump_name(input);
        extracted = work && name ? snx_path_join(work, name) : NULL;
        free(work);
        free(name);
    }
    if (!extracted) return 1; /* work_folder said why */
    unsigned flags = recompile ? SNX_CTR_RECOMPILE | SNX_CTR_COMPILE_ONLY : 0;
    if (!extract_container && !extract_only) { /* the CIA read where it lies, the folder keeping only the title's state */
        int status = play_cia(input, extracted, model, max_blocks, flags, error, sizeof error);
        free(extracted);
        return status;
    }
    /* like the Switch driver: a finished extraction is used again, the extractor publishes only complete ones */
    if (holds_extraction(extracted))
        printf("Using the extraction in %s\n", extracted);
    else {
        printf("Extracting %s\n", input);
        char* published = NULL;
        SnxCtrExtractResult result = extract_into(input, extracted, &published, error, sizeof error);
        free(extracted);
        if (result != SNX_CTR_EXTRACT_OK) {
            fprintf(stderr, "scarletnx-3ds: %s\n", error);
            return result == SNX_CTR_EXTRACT_UNSUPPORTED ? 3 : 1;
        }
        extracted = published;
        printf("Extracted %s\n", extracted);
    }
    if (extract_only) {
        free(extracted);
        return 0;
    }
    int status = snx_ctr_aot_run(extracted, NULL, model, max_blocks, flags, error, sizeof error);
    free(extracted);
    if (status) fprintf(stderr, "scarletnx-3ds: %s\n", error);
    return status;
}
