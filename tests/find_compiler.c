/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_find_compiler: the compiler a 3DS game's native link runs. The build's own where it is on the machine, else a
   clang beside the program, else one on PATH; none of them gives NULL and each place looked at, as a release runs on
   machines without the build's compiler (a Windows one had the CI's D:/a/_temp/... path built in) */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define CLANG "clang.exe"
#else
#define CLANG "clang"
#endif

static int g_failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
static void set_path(const char* value) {
#ifdef _WIN32
    _putenv_s("PATH", value);
#else
    setenv("PATH", value, 1);
#endif
}
/* an executable file named name in folder, its path owned */
static char* program(const char* folder, const char* name) {
    char* path = snx_path_join(folder, name);
    check(!snx_mkdirs(folder) && !snx_write_file(path, "x", 1) && !snx_make_executable(path), path);
    return path;
}
/* found is the same file as want */
static int same(const char* found, const char* want) {
    char *a = found ? snx_absolute(found) : NULL, *b = snx_absolute(want);
    int is = a && b && !strcmp(a, b);
    free(a);
    free(b);
    return is;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *built, *beside, *on_path, *gone, *found, *path_was, error[512];
    const char* was = getenv("PATH");
    if (!root) {
        fprintf(stderr, "FAIL: no temporary folder\n");
        return 1;
    }
    path_was = snx_strdup(was ? was : "");
    built = program(root, "built-cc");
    beside = snx_path_join(root, "program");
    on_path = snx_path_join(root, "path");
    gone = snx_path_join(root, "not-here/clang");
    free(program(beside, CLANG));
    free(program(on_path, CLANG));
    set_path(on_path);

    /* the build's own, where it is here: before any other */
    found = snx_find_compiler(built, beside, error, sizeof error);
    check(same(found, built), "the build's compiler, when it is on this machine");
    free(found);
    /* not here (another machine's path): the clang beside the program */
    found = snx_find_compiler(gone, beside, error, sizeof error);
    {
        char* want = snx_path_join(beside, CLANG);
        check(same(found, want), "a clang beside the program, the build's not being here");
        free(want);
    }
    free(found);
    /* none beside it: the one on PATH */
    found = snx_find_compiler(gone, root, error, sizeof error);
    {
        char* want = snx_path_join(on_path, CLANG);
        check(same(found, want), "a clang on PATH, none being beside the program");
        free(want);
    }
    free(found);
    /* none anywhere: NULL, each place named */
    set_path(root);
    error[0] = 0;
    found = snx_find_compiler(gone, root, error, sizeof error);
    check(!found, "no compiler anywhere gives none");
    check(strstr(error, gone) && strstr(error, root) && strstr(error, CLANG) && strstr(error, "PATH"),
          "the error names the build's compiler, the program's folder, clang and PATH");
    if (!strstr(error, gone)) fprintf(stderr, "error: %s\n", error);
    free(found);

    set_path(path_was);
    check(!snx_remove_tree(root), "temporary folder removed");
    free(path_was);
    free(built);
    free(beside);
    free(on_path);
    free(gone);
    free(root);
    if (g_failures) return 1;
    puts("find_compiler: ok");
    return 0;
}
