/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_IMAGE_H
#define SNX_VITA_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The game's executable (eboot.bin) as the runtime loads it and the compiler compiles it: a fake SELF (homebrew's,
   with plain segments) or the ELF itself, an ET_SCE_RELEXEC module placed at VITA_LOAD_BASE with its SCE relocations
   applied. Each function it imports has a stub in its text, which the module loader fills on the console; here the
   stub becomes "svc #index; bx lr" (A32), index the import's in imports[], which the runtime answers. The placement
   is the same every time, so the compiled blocks, which hold guest addresses, match the image the runtime loads */
#define VITA_LOAD_BASE  0x81000000u
#define VITA_SEGMENTS   4u
/* the SVC immediates at and above this are the runtime's own (vita/runtime/process.h), below it an import's index */
#define VITA_SVC_RUNTIME 0x00ff0000u

typedef struct {
    uint32_t address, size; /* in the guest, size the memory size (BSS included) */
    uint32_t flags;         /* ELF's PF_X (1), PF_W (2), PF_R (4) */
    unsigned char* bytes;   /* size bytes, zero past the file's */
} VitaSegment;
typedef struct {
    uint32_t nid, library_nid, stub;
    char library[32];
} VitaImport;
typedef struct {
    VitaSegment segment[VITA_SEGMENTS];
    unsigned segment_count;
    char name[28];
    /* guest addresses, a function's with its Thumb bit; 0 for none */
    uint32_t module_start, module_stop, process_param;
    VitaImport* imports;
    uint32_t import_count;
    /* code pointers the relocations wrote (Thumb ones, with their bit): where discovery starts beside the exports */
    uint32_t* seeds;
    size_t seed_count;
    unsigned char identity[32]; /* SHA-256 of the image as loaded */
} VitaImage;

/* eboot at path, loaded and relocated. False with error filled: an encrypted SELF (a retail game's, which needs its
   keys), an ET_SCE_EXEC module (not yet), a relocation it doesn't know */
bool vita_image_load(const char* path, VitaImage* image, char* error, size_t error_size);
void vita_image_free(VitaImage* image);
/* the segment holding [address, address + size), NULL for none */
const VitaSegment* vita_image_segment(const VitaImage* image, uint32_t address, uint32_t size);
#endif
