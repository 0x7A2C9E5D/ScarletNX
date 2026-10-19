/* Fingerprint the generated-C compatibility interface without LLVM. */
#include "common/cpu/aarch64/a64.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "step")) snx_a64_interp(stdout, 1);
    else if (!strcmp(argv[1], "checked")) snx_a64_interp_checked(stdout);
    else if (!strcmp(argv[1], "normal")) snx_a64_interp(stdout, 0);
    else return 2;
    return ferror(stdout);
}
