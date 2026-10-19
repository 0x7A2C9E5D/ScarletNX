/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* link_macho.c's objects, compiled for arm64 macOS twice: PART 1 calls into PART 2 (branches, GOT loads, pointers in
   data to rebase, a weak import nothing defines, zerofill, constants), as LLVM's modules do */
#if PART == 1
extern int counter;
int helper(int x);
extern int missing(void) __attribute__((weak_import));
const char* names[] = {"zero", "one", "two"};
int (*fns[])(int) = {helper, helper};
double scale[] = {1.5, 2.25};
unsigned long long zeroes[4096];
unsigned long long test_entry(void);
unsigned long long test_entry(void) {
    zeroes[4095] = 3;
    return (unsigned long long)(names[2][1] + fns[1](2) + counter + (missing ? 1 : 2)) + zeroes[4095] +
           (unsigned long long)scale[1];
}
#else
int counter = 10;
int helper(int x);
int helper(int x) { return x * 3; }
#endif
