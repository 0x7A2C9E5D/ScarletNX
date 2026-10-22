/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/loader/ctr_image.h"
#include "3ds/loader/ctr_loader.h"
#include "common/utilities/common.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {
    bool room_netplay;
} g_image;
void snx_ctr_image_netplay_room(bool enabled) { g_image.room_netplay = enabled; }

static uint32_t le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool read_exact(const char* root, const char* name, void* out, size_t size) {
    char* path = snx_path_join(root, name);
    FILE* f = !snx_is_link(path) && snx_is_file(path) ? snx_fopen(path, "rb") : NULL;
    free(path);
    if (!f) return false;
    bool ok = fread(out, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    if (fclose(f)) ok = false;
    return ok;
}
void snx_ctr_image_free(SnxCtrImage* image) {
    if (image) {
        free(image->code);
        memset(image, 0, sizeof *image);
    }
}
/* the image an ExHeader describes, its expanded .code read exact and bounded by code(source, out, size) */
static bool image_parse(const unsigned char* ex, bool (*code)(const void*, unsigned char*, size_t), const void* source,
                        SnxCtrImage* image, char* error, size_t error_size) {
    image->stack_size = le32(ex + 0x1c);
    image->bss_size = le32(ex + 0x3c);
    image->title_id = (uint64_t)le32(ex + 0x200) | (uint64_t)le32(ex + 0x204) << 32;
    memcpy(image->name, ex, sizeof image->name);
    image->main_priority = ex[0x20f];
    for (unsigned i = 0; i < 28; ++i) image->kernel_caps[i] = le32(ex + 0x370 + i * 4);
    const char* reason = "invalid ExHeader main thread priority";
    if (image->main_priority > 63) goto fail;
    const unsigned offsets[] = {0x10, 0x20, 0x30};
    uint64_t sizes[3], total = 0;
    reason = "invalid extracted process segment/BSS/stack layout";
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned char* p = ex + offsets[i];
        SnxCtrSegment* s = &image->segment[i];
        *s = (SnxCtrSegment){le32(p), le32(p + 4), le32(p + 8)};
        uint64_t bytes = (uint64_t)s->pages * 4096;
        sizes[i] = bytes + (i == 2 ? ((uint64_t)image->bss_size + 4095) & ~4095ull : 0);
        /* Match extraction: some homebrew .data byte counts include BSS. */
        uint64_t initialized = s->bytes;
        if (i == 2 && initialized > bytes && initialized >= image->bss_size) initialized -= image->bss_size;
        if (initialized > bytes ||
            (sizes[i] && ((s->address & 4095) || s->address < 0x100000 || (uint64_t)s->address + sizes[i] > 0x4000000)))
            goto fail;
        for (unsigned j = 0; j < i; ++j)
            if (sizes[i] && sizes[j] && s->address < (uint64_t)image->segment[j].address + sizes[j] &&
                image->segment[j].address < (uint64_t)s->address + sizes[i])
                goto fail;
        total += bytes;
    }
    if (!image->segment[0].bytes || (image->segment[0].bytes & 3) || total > 0x4000000 || !image->stack_size ||
        image->stack_size > 0x800000)
        goto fail;
    image->code_size = (size_t)total;
    image->code = malloc(image->code_size);
    reason = "cannot read exact bounded expanded .code";
    if (!image->code || !code(source, image->code, image->code_size)) goto fail;
    /* NSMB2's local-wireless window is too short for internet rooms. Keep the
       original wire history so increasing it does not overflow UdsMessage.
       The modified image identity below forces matching AOT/JIT translation. */
    const char* disable = getenv("SNX_3DS_NSMB2_NETPLAY");
    if (g_image.room_netplay && !(disable && !strcmp(disable, "0")) && image->title_id == 0x000400000007ae00ull &&
        image->code_size == 0x503000) {
        const unsigned at = 0x485afa;
        const unsigned char settings[8] = {32, 3, 2, 120, 3, 128, 3, 128};
        const unsigned sites[5] = {0x4ec10, 0x4ec24, 0x4ec38, 0x4ec98, 0x4ecc8};
        const uint32_t expected[5] = {0xe1d411b2, 0xe1d411b2, 0xe1d411b2, 0xe1d401b2, 0xe1d491b2};
        const uint32_t replacement[5] = {0xe3a01003, 0xe3a01003, 0xe3a01003, 0xe3a00003, 0xe3a09003};
        bool matches = !memcmp(image->code + at, settings, sizeof settings);
        for (unsigned i = 0; i < 5; ++i) matches = matches && le32(image->code + sites[i]) == expected[i];
        const unsigned char original_sha256[32] = {0x74, 0x48, 0xf9, 0x4e, 0x05, 0x7b, 0x17, 0x09, 0xfc, 0xd5, 0xd6,
                                                   0x1f, 0x7e, 0x85, 0x64, 0x19, 0x81, 0xa6, 0x37, 0x34, 0x64, 0xf3,
                                                   0x2c, 0x19, 0xbd, 0xe6, 0xc6, 0xe4, 0xb4, 0x59, 0xd9, 0x2a};
        unsigned char digest[32];
        unsigned digest_size = 0;
        matches = matches && EVP_Digest(image->code, image->code_size, digest, &digest_size, EVP_sha256(), NULL) == 1 &&
                  digest_size == sizeof digest && !memcmp(digest, original_sha256, sizeof digest);
        if (matches) {
            image->code[at] = 64;
            image->code[at + 1] = 12;
            for (unsigned i = 0; i < 5; ++i)
                for (unsigned j = 0; j < 4; ++j) image->code[sites[i] + j] = (unsigned char)(replacement[i] >> (8 * j));
            fprintf(stderr, "3ds netplay: NSMB2 USA v0 input window=12 frames, wire history=6\n");
        } else
            fprintf(stderr, "3ds netplay: NSMB2 image is not the verified USA v0 revision; patch skipped\n");
    }
    static const char domain[] = "ScarletNX CTR image identity v1";
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    bool hashed = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
                  EVP_DigestUpdate(hash, domain, sizeof domain) == 1 && EVP_DigestUpdate(hash, ex, 0x800) == 1 &&
                  EVP_DigestUpdate(hash, image->code, image->code_size) == 1 &&
                  EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    reason = "cannot hash the image with OpenSSL";
    if (!hashed) goto fail;
    return true;
fail:
    if (error_size) snprintf(error, error_size, "%s", reason);
    snx_ctr_image_free(image);
    return false;
}
static bool extracted_code(const void* directory, unsigned char* out, size_t size) {
    return read_exact(directory, "exefs/.code", out, size);
}
/* why an extraction's folder is refused: a link is never followed */
static const char* not_folder(const char* path) {
    return snx_is_link(path) ? "is a link" : !snx_is_dir(path) ? "is not a folder" : NULL;
}
bool snx_ctr_image_load(const char* directory, SnxCtrImage* image, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (!image) return false;
    memset(image, 0, sizeof *image);
    if (!directory) {
        snprintf(error, error_size, "no extraction folder given");
        return false;
    }
    unsigned char ex[0x800];
    char* exefs = snx_path_join(directory, "exefs");
    const char *why = not_folder(directory), *refused = directory;
    if (!why && (why = not_folder(exefs))) refused = exefs;
    if (why) snprintf(error, error_size, "%s %s", refused, why);
    free(exefs);
    if (why) return false;
    if (!read_exact(directory, "exheader.bin", ex, sizeof ex)) {
        snprintf(error, error_size, "%s/exheader.bin is not a file of %u bytes", directory, (unsigned)sizeof ex);
        return false;
    }
    return image_parse(ex, extracted_code, directory, image, error, error_size);
}
/* the .code snx_ctr_cia_open expanded: exactly the size the ExHeader's pages give */
static bool cia_code(const void* cia, unsigned char* out, size_t size) {
    size_t have;
    const unsigned char* code = snx_ctr_cia_code(cia, &have);
    if (have != size) return false;
    memcpy(out, code, size);
    return true;
}
bool snx_ctr_image_from_cia(const SnxCtrCia* cia, SnxCtrImage* image, char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (!image) return false;
    memset(image, 0, sizeof *image);
    if (!cia) {
        if (error_size) snprintf(error, error_size, "no CIA to read the image from");
        return false;
    }
    return image_parse(snx_ctr_cia_exheader(cia), cia_code, cia, image, error, error_size);
}
