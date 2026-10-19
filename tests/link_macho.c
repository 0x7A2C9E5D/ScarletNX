/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_link_macho, where no dyld can load what it writes: the dylib checked as macOS would. Segments on 16 KiB pages,
   in order, __LINKEDIT last; the rebases inside __DATA, onto addresses in the image; the export trie finding the
   entry in __text; the ad hoc signature last, with each 4 KiB page's SHA-256 as OpenSSL computes it. A dump of the
   output is written to argv[3] for llvm-objdump --macho. Usage: link_macho part1.o part2.o out.dylib */
#include "common/cpu/recompiler/llvm/link.h"
#include <assert.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const unsigned char* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint32_t be32(const unsigned char* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t uleb(const unsigned char** p) {
    uint64_t v = 0;
    unsigned shift = 0;
    do {
        v |= (uint64_t)(**p & 0x7F) << shift;
        shift += 7;
    } while (*(*p)++ & 0x80);
    return v;
}

static unsigned char* slurp(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    unsigned char* data;
    long n;
    assert(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char*)malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return data;
}

typedef struct {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
} Segment;

/* the trie's address of name, or 0 */
static uint64_t trie_find(const unsigned char* trie, size_t size, const char* name) {
    const unsigned char* node = trie;
    for (;;) {
        const unsigned char* p = node;
        uint64_t info = uleb(&p);
        unsigned children, i;
        int next = 0;
        if (!*name) {
            if (!info) return 0;
            assert(*p++ == 0); /* EXPORT_SYMBOL_FLAGS_KIND_REGULAR */
            return uleb(&p);
        }
        p += info;
        children = *p++;
        for (i = 0; i < children && !next; ++i) {
            size_t n = strlen((const char*)p);
            uint64_t offset;
            int match = !strncmp((const char*)p, name, n);
            p += n + 1;
            offset = uleb(&p);
            if (match) {
                assert(offset < size);
                name += n;
                node = trie + offset;
                next = 1;
            }
        }
        if (!next) return 0;
    }
}

int main(int argc, char** argv) {
    SnxLinkInput inputs[2];
    SnxLinkOutput out;
    Segment segments[4], *text = NULL, *data = NULL, *linkedit = NULL;
    const char* exports[] = {"test_entry"};
    char error[512];
    const unsigned char *d, *c, *trie = NULL, *rebase = NULL, *p, *end;
    uint64_t text_address = 0, text_size = 0, entry, address = 0, symbol_value = 0;
    uint32_t ncmds, i, nsegments = 0, trie_size = 0, rebase_size = 0, signature_offset = 0, signature_size = 0,
                    nslots, k;
    unsigned rebases = 0;
    FILE* f;
    assert(argc == 4);
    inputs[0].data = slurp(argv[1], &inputs[0].size);
    inputs[0].name = argv[1];
    inputs[1].data = slurp(argv[2], &inputs[1].size);
    inputs[1].name = argv[2];
    /* the entry's own object alone leaves helper and counter undefined */
    assert(snx_link(inputs, 1, exports, 1, "test.dylib", &out, error, sizeof(error)) && strstr(error, "undefined"));
    if (snx_link(inputs, 2, exports, 1, "test.dylib", &out, error, sizeof(error))) {
        fprintf(stderr, "link: %s\n", error);
        return 1;
    }
    f = fopen(argv[3], "wb");
    assert(f && fwrite(out.data, 1, out.size, f) == out.size);
    fclose(f);
    d = out.data;
    assert(rd32(d) == 0xFEEDFACFu && rd32(d + 4) == 0x0100000Cu && rd32(d + 12) == 6); /* arm64 MH_DYLIB */
    ncmds = rd32(d + 16);
    c = d + 32;
    for (i = 0; i < ncmds; ++i, c += rd32(c + 4)) {
        uint32_t cmd = rd32(c);
        if (cmd == 0x19) { /* LC_SEGMENT_64 */
            Segment* s = &segments[nsegments++];
            assert(nsegments <= 4);
            memcpy(s->name, c + 8, 16);
            s->name[16] = 0;
            s->vmaddr = rd64(c + 24);
            s->vmsize = rd64(c + 32);
            s->fileoff = rd64(c + 40);
            s->filesize = rd64(c + 48);
            if (!strcmp(s->name, "__TEXT")) {
                text = s;
                assert(!strcmp((const char*)c + 72, "__text"));
                text_address = rd64(c + 72 + 32);
                text_size = rd64(c + 72 + 40);
            }
            if (!strcmp(s->name, "__DATA")) data = s;
            if (!strcmp(s->name, "__LINKEDIT")) linkedit = s;
        } else if (cmd == 0x80000022u) { /* LC_DYLD_INFO_ONLY */
            rebase = d + rd32(c + 8);
            rebase_size = rd32(c + 12);
            assert(!rd32(c + 20) && !rd32(c + 28) && !rd32(c + 36)); /* no binds */
            trie = d + rd32(c + 40);
            trie_size = rd32(c + 44);
        } else if (cmd == 0x2) { /* LC_SYMTAB: the export, last and external */
            const unsigned char* last = d + rd32(c + 8) + 16 * (size_t)(rd32(c + 12) - 1);
            assert(!strcmp((const char*)d + rd32(c + 16) + rd32(last), "_test_entry") && last[4] == 0xF);
            symbol_value = rd64(last + 8);
        } else if (cmd == 0x1D) { /* LC_CODE_SIGNATURE */
            signature_offset = rd32(c + 8);
            signature_size = rd32(c + 12);
        }
    }
    /* segments: __TEXT from the start of the file, every one on pages, in order, __LINKEDIT to the end of the file */
    assert(nsegments == 3 && text == &segments[0] && data == &segments[1] && linkedit == &segments[2]);
    assert(!text->fileoff && !text->vmaddr && 32 + rd32(d + 20) <= text_address);
    for (i = 0; i < nsegments; ++i) {
        assert(!(segments[i].vmaddr % 0x4000) && !(segments[i].fileoff % 0x4000) && !(segments[i].vmsize % 0x4000));
        assert(segments[i].filesize <= segments[i].vmsize);
        if (i) assert(segments[i].vmaddr >= segments[i - 1].vmaddr + segments[i - 1].vmsize);
    }
    assert(linkedit->fileoff + linkedit->filesize == out.size);
    /* rebases: pointers in __DATA to addresses in the image (the GOT's, the tables') */
    assert(rebase && rebase_size);
    for (p = rebase, end = rebase + rebase_size; p < end && *p;) {
        unsigned op = *p & 0xF0, imm = *p++ & 0x0F;
        uint64_t n;
        switch (op) {
            case 0x10: assert(imm == 1); break; /* SET_TYPE_IMM, pointers */
            case 0x20: /* SET_SEGMENT_AND_OFFSET_ULEB, in __DATA */
                assert(imm == 1);
                address = data->vmaddr + uleb(&p);
                break;
            case 0x30: address += uleb(&p); break;
            case 0x60:
                for (n = uleb(&p); n--; address += 8, ++rebases) {
                    uint64_t target = rd64(d + data->fileoff + (address - data->vmaddr));
                    assert(address >= data->vmaddr && address + 8 <= data->vmaddr + data->filesize);
                    assert(target < data->vmaddr + data->vmsize);
                }
                break;
            default: assert(!"unexpected rebase opcode");
        }
    }
    assert(rebases >= 5); /* names, fns, and the GOT's counter */
    /* the export */
    entry = trie_find(trie, trie_size, "_test_entry");
    assert(entry && entry == symbol_value && entry >= text_address && entry < text_address + text_size);
    assert(!trie_find(trie, trie_size, "_helper") && !trie_find(trie, trie_size, "test_entry"));
    /* the signature: the last thing in the file, hashing every page before it */
    assert(signature_offset && signature_offset + signature_size == out.size && !(signature_offset % 16));
    p = d + signature_offset;
    assert(be32(p) == 0xFADE0CC0u && be32(p + 4) == signature_size && be32(p + 8) == 1 && be32(p + 12) == 0);
    c = p + be32(p + 16);
    assert(be32(c) == 0xFADE0C02u && be32(c + 8) >= 0x20400 && (be32(c + 12) & 0x20002) == 0x20002);
    assert(be32(c + 32) == signature_offset && c[36] == 32 && c[37] == 2 && c[39] == 12);
    assert(!strcmp((const char*)c + be32(c + 20), "test.dylib"));
    nslots = be32(c + 28);
    assert(nslots == (signature_offset + 4095) / 4096);
    for (k = 0; k < nslots; ++k) {
        unsigned char digest[32];
        unsigned length = 0;
        size_t at = (size_t)k * 4096, n = signature_offset - at < 4096 ? signature_offset - at : 4096;
        assert(EVP_Digest(d + at, n, digest, &length, EVP_sha256(), NULL) && length == 32);
        assert(!memcmp(c + be32(c + 16) + 32 * (size_t)k, digest, 32));
    }
    printf("link_macho: %lu bytes, %u rebases, entry at 0x%llx, %u pages signed\n", (unsigned long)out.size, rebases,
           (unsigned long long)entry, nslots);
    free(out.data);
    free((void*)inputs[0].data);
    free((void*)inputs[1].data);
    return 0;
}
