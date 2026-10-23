/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's side of a flat guest space (common/memory/host.c), on every host the CI builds: one object mapped at two
   places of a reservation is the same memory, a commit is zeros of its own, and a page unmapped goes back to the
   reservation, to be mapped again elsewhere in the object */
#include "common/memory/host.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const unsigned rw = SNX_HOST_READ | SNX_HOST_WRITE;
    size_t page = snx_host_page();
    SnxHostMemory memory;
    unsigned char* base;
    if (!page) {
        fputs("host_memory: this host makes no views (snx_host_page is 0)\n", stderr);
        return 1;
    }
    assert(snx_host_memory_new(&memory, 2 * page));
    base = snx_host_reserve(8 * page);
    assert(base);
    /* the object at 0, its first page again at 4 */
    assert(snx_host_map(&memory, base, 0, 0, 2 * page, rw));
    assert(snx_host_map(&memory, base, 4 * page, 0, page, rw));
    base[5] = 42;
    assert(base[4 * page + 5] == 42 && memory.view[5] == 42);
    base[5 * page - 1] = 7;
    assert(base[page - 1] == 7);
    memory.view[page] = 9;
    assert(base[page] == 9);
    /* pages 2 and 3 zeros that are no view of it */
    assert(snx_host_commit(base, 2 * page, 2 * page));
    assert(!base[2 * page] && !base[4 * page - 1]);
    base[2 * page] = 5;
    assert(base[2 * page] == 5 && memory.view[0] != 5 && memory.view[page] == 9);
    assert(snx_host_protect(base, 0, page, SNX_HOST_READ) && base[5] == 42);
    /* page 4 back to the reservation, then the object's second page there */
    assert(snx_host_unmap(base, 4 * page, page));
    assert(snx_host_map(&memory, base, 4 * page, page, page, rw));
    assert(base[4 * page] == 9);
    snx_host_release(base, 8 * page);
    snx_host_memory_free(&memory);
    printf("host_memory: ok, %zu byte pages\n", page);
    return 0;
}
