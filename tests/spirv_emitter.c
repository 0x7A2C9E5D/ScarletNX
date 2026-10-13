/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "spirv_emitter_fixture.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    size_t words = 0;
    uint32_t* code = snx_test_spirv_compute(&words);
    FILE* file = fopen(argv[1], "wb");
    if (!file) {
        free(code);
        return 1;
    }
    int ok = fwrite(code, sizeof(uint32_t), words, file) == words;
    if (fclose(file)) ok = 0;
    free(code);
    return ok ? 0 : 1;
}
