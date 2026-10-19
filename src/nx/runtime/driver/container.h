/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Switch dump (XCI or NSP, or their decrypted DXCI and DNSP) opened with hactool in this process, its keys loaded
   and its NCAs picked as -x extracts them: what the extraction, the recompiler (a dump given to -r) and the runtime (a
   game whose dump is not extracted, read in place) share */
#ifndef SNX_CONTAINER_H
#define SNX_CONTAINER_H

#include "common/utilities/common.h"
#include "nx/runtime/driver/hactool/snx_hac.h"

/* an NCA where it lies: a container's number (snx_hac_open) and its entry, found 0 for none */
typedef struct {
    int found, container;
    size_t entry;
    SnxU64 size, title_id;
} SnxNcaRef;

typedef struct {
    SnxHac* hac;
    char *keys, *title_keys; /* where they were looked for */
    int has_keys;
    int base, update; /* the containers, update -1 without one */
    /* the base's Program NCA, the update's (the patch an NSP of both has, else found 0), and the Control NCA, the
       update's when it has one */
    SnxNcaRef program, patch, control;
} SnxDump;

/* prod.keys or title.keys: given, else in repo (NULL for none), else in ~/.switch; the path in repo when it is in
   neither. NULL when out of memory */
char* snx_find_keys(const char* given, const char* name, const char* repo);
/* 0 with *xci set for an NSP (0) or an XCI (1), else nonzero, said why */
int snx_container_kind(const char* path, int* xci);
/* 1 for a file whose header is an NSP's or an XCI's, else 0, saying nothing */
int snx_is_dump(const char* path);
/* what failed, with hactool's last error after it when it said one: 1 */
int snx_hac_error(const char* what);
/* base, and update unless it is NULL, opened and their NCAs picked, with the keys given or found (snx_find_keys, the
   ones given must be there). say prints which dump is read. 0, else nonzero, said why; snx_dump_close either way */
int snx_dump_open(SnxDump* dump, const char* base, const char* update, const char* keys, const char* title_keys,
                  const char* repo, int say);
void snx_dump_close(SnxDump* dump);
/* the program's ExeFS and RomFS read in place, an update's over the base, or NULL, said why */
SnxHacFs* snx_dump_fs(SnxDump* dump);
/* a file at the root of a RomFS read in place (control.nacp in the Control NCA's), malloc'd, or NULL */
unsigned char* snx_romfs_file(SnxHacFs* fs, const char* name, size_t* size);
/* the Control NCA's control.nacp, malloc'd, or NULL with why in *why */
unsigned char* snx_dump_nacp(SnxDump* dump, size_t* size, const char** why);

#endif
