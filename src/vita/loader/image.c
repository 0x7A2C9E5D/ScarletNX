/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SELF container, the SCE ELF module format (module info, import and export tables, the SCE_RELA relocations)
   as the Vita's homebrew toolchain writes them (vita-toolchain's vita-elf-create, and its headers, MIT), and the PS
   Vita developer wiki's SELF pages */
#include "vita/loader/image.h"
#include "vita/loader/inflate.h"
#include "common/utilities/common.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ET_SCE_EXEC    0xfe00u
#define ET_SCE_RELEXEC 0xfe04u
#define PT_LOAD        1u
#define PT_SCE_RELA    0x60000000u
#define NID_MODULE_START  0x935cd196u
#define NID_MODULE_STOP   0x79f8e492u
#define NID_PROCESS_PARAM 0x70fba1e7u
#define PAGE 0x1000u

typedef struct {
    const unsigned char* data;
    size_t size;
    uint32_t type, vaddr, filesz, memsz, flags, align;
    unsigned char* owned; /* a decompressed segment's bytes */
} Program;
typedef struct {
    VitaImage* image;
    uint32_t base[16]; /* each program header's load address, by its index (a relocation's segment), 0 for none */
    int loaded[16];    /* the image segment of each program header, -1 for none */
    size_t seed_capacity;
    char* error;
    size_t error_size;
} Loader;

static uint32_t le16(const unsigned char* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char* p) { return le16(p) | le16(p + 2) << 16; }
static uint64_t le64(const unsigned char* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put16(unsigned char* p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }

const VitaSegment* vita_image_segment(const VitaImage* image, uint32_t address, uint32_t size) {
    for (unsigned i = 0; i < image->segment_count; ++i) {
        const VitaSegment* s = &image->segment[i];
        if (address >= s->address && (uint64_t)address + size <= (uint64_t)s->address + s->size) return s;
    }
    return NULL;
}
static unsigned char* at(VitaImage* image, uint32_t address, uint32_t size) {
    const VitaSegment* s = vita_image_segment(image, address, size);
    return s ? s->bytes + (address - s->address) : NULL;
}
static bool word(VitaImage* image, uint32_t address, uint32_t* value) {
    const unsigned char* p = at(image, address, 4);
    if (p) *value = le32(p);
    return p != NULL;
}
static bool fail(Loader* l, const char* format, uint32_t a, uint32_t b) {
    snprintf(l->error, l->error_size, format, a, b);
    return false;
}
static void seed(Loader* l, uint32_t pointer) {
    VitaImage* image = l->image;
    const VitaSegment* s = vita_image_segment(image, pointer & ~1u, 2);
    if (!(pointer & 1) || !s || !(s->flags & 1)) return; /* a Thumb function's address in executable memory */
    if (image->seed_count == l->seed_capacity) {
        size_t capacity = l->seed_capacity ? l->seed_capacity * 2 : 1024;
        uint32_t* grown = realloc(image->seeds, capacity * sizeof *grown);
        if (!grown) return;
        image->seeds = grown, l->seed_capacity = capacity;
    }
    image->seeds[image->seed_count++] = pointer;
}

/* One relocation: code (ARM ELF's relocation types) at P in segment datseg, the symbol's segment's base S (0 for
   segment 15, an absolute value) plus A. A PC-relative one inside one segment changes nothing, as the segment moves
   whole; across segments it is computed again */
static bool relocate(Loader* l, unsigned code, unsigned symseg, unsigned datseg, uint32_t offset, uint32_t addend) {
    if (datseg >= 16 || l->loaded[datseg] < 0 || (symseg != 15 && (symseg >= 16 || l->loaded[symseg] < 0)))
        return fail(l, "relocation between segments %u and %u, which aren't loaded", symseg, datseg);
    uint32_t S = symseg == 15 ? 0 : l->base[symseg], P = l->base[datseg] + offset, value = S + addend;
    bool same = symseg == datseg;
    unsigned char* p = at(l->image, P, code == 0 || code == 40 ? 0 : 4);
    if (!p) return fail(l, "relocation at 0x%08x (type %u) outside its segment", P, code);
    switch (code) {
        case 0:  /* R_ARM_NONE */
        case 40: /* R_ARM_V4BX: BX is the architecture's on ARMv7 */
            return true;
        case 2:  /* R_ARM_ABS32 */
        case 38: /* R_ARM_TARGET1, ABS32 on the Vita */
            put32(p, value);
            seed(l, value);
            return true;
        case 3:  /* R_ARM_REL32 */
        case 41: /* R_ARM_TARGET2, REL32 on the Vita */
            if (!same) put32(p, value - P);
            return true;
        case 42: /* R_ARM_PREL31 */
            if (!same) put32(p, (le32(p) & 0x80000000u) | ((value - P) & 0x7fffffffu));
            return true;
        case 10: /* R_ARM_THM_CALL */
        case 30: /* R_ARM_THM_JUMP24 */
        case 28: /* R_ARM_CALL */
        case 29: /* R_ARM_JUMP24 */
            if (!same) return fail(l, "a branch between segments (relocation type %u at 0x%08x)", code, P);
            return true;
        case 43: /* R_ARM_MOVW_ABS_NC */
        case 44: { /* R_ARM_MOVT_ABS */
            uint32_t v = code == 44 ? value >> 16 : value & 0xffff, w = le32(p);
            put32(p, (w & 0xfff0f000u) | ((v & 0xf000) << 4) | (v & 0x0fff));
            return true;
        }
        case 47: /* R_ARM_THM_MOVW_ABS_NC */
        case 48: { /* R_ARM_THM_MOVT_ABS: imm16 = imm4:i:imm3:imm8 over the two halfwords */
            uint32_t v = code == 48 ? value >> 16 : value & 0xffff, upper = le16(p), lower = le16(p + 2);
            upper = (upper & 0xfbf0) | ((v >> 12) & 15) | (((v >> 11) & 1) << 10);
            lower = (lower & 0x8f00) | (((v >> 8) & 7) << 12) | (v & 0xff);
            put16(p, upper), put16(p + 2, lower);
            if (code == 47) seed(l, value);
            return true;
        }
        default: return fail(l, "unknown relocation type %u at 0x%08x", code, P);
    }
}
static bool relocate_all(Loader* l, const unsigned char* r, size_t size) {
    for (size_t pos = 0; pos < size;) {
        if (size - pos < 8) return fail(l, "truncated relocation at %u of %u bytes", (uint32_t)pos, (uint32_t)size);
        uint32_t type = le32(r + pos), offset, addend, code2 = 0, dist2 = 0;
        if (type & 15) { /* the short form: the offset split over the two words, a 12-bit addend */
            uint32_t second = le32(r + pos + 4);
            offset = (type >> 20) | (second & 0xfffff) << 12;
            addend = second >> 20;
            pos += 8;
        } else {
            if (size - pos < 12) return fail(l, "truncated relocation at %u of %u bytes", (uint32_t)pos, (uint32_t)size);
            addend = le32(r + pos + 4);
            offset = le32(r + pos + 8);
            code2 = (type >> 20) & 0xff;
            dist2 = (type >> 28) & 15;
            pos += 12;
        }
        unsigned code = (type >> 8) & 0xff, symseg = (type >> 4) & 15, datseg = (type >> 16) & 15;
        if (!relocate(l, code, symseg, datseg, offset, addend)) return false;
        /* a second relocation of the same symbol dist2 halfwords on (a MOVW's MOVT) */
        if (code2 && !relocate(l, code2, symseg, datseg, offset + dist2 * 2, addend)) return false;
    }
    return true;
}
/* Fills each imported function's stub: "svc #index; bx lr", in the stub's instruction set */
static bool imports(Loader* l, uint32_t top, uint32_t end) {
    VitaImage* image = l->image;
    uint32_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t index = 0;
        for (uint32_t p = top; p < end;) {
            const unsigned char* e = at(image, p, 0x24);
            if (!e || (le16(e) != 0x24 && le16(e) != 0x34) || !at(image, p, le16(e)))
                return fail(l, "bad import table entry at 0x%08x", p, 0);
            uint32_t functions = le16(e + 6), variables, library_nid, name, nids, entries;
            if (le16(e) == 0x34) { /* the older layout, with TLS variables */
                variables = le16(e + 8) + le16(e + 10);
                library_nid = le32(e + 16), name = le32(e + 20), nids = le32(e + 28), entries = le32(e + 32);
            } else {
                variables = le32(e + 8);
                library_nid = le32(e + 12), name = le32(e + 16), nids = le32(e + 20), entries = le32(e + 24);
            }
            if (variables) return fail(l, "the module imports %u variables (at 0x%08x), which isn't supported yet",
                                       variables, p);
            for (uint32_t i = 0; i < functions; ++i, ++index) {
                uint32_t nid, stub;
                if (!word(image, nids + i * 4, &nid) || !word(image, entries + i * 4, &stub))
                    return fail(l, "bad import table at 0x%08x", p, 0);
                if (!pass) continue;
                VitaImport* import = &image->imports[index];
                import->nid = nid, import->library_nid = library_nid, import->stub = stub;
                for (unsigned c = 0; c + 1 < sizeof import->library; ++c) {
                    const unsigned char* ch = at(image, name + c, 1);
                    if (!ch || !*ch) break;
                    import->library[c] = (char)*ch;
                }
                unsigned char* code = at(image, stub & ~1u, 4);
                if (!code) return fail(l, "import stub 0x%08x outside the module", stub, 0);
                if (stub & 1) {
                    if (index > 255) return fail(l, "Thumb import stub 0x%08x for import %u", stub, index);
                    put16(code, 0xdf00 | index); /* svc #index */
                    put16(code + 2, 0x4770);     /* bx lr */
                } else {
                    if (!at(image, stub, 8)) return fail(l, "import stub 0x%08x outside the module", stub, 0);
                    put32(code, 0xef000000u | index); /* svc #index */
                    put32(code + 4, 0xe12fff1eu);     /* bx lr */
                }
            }
            p += le16(e);
        }
        if (!pass) {
            count = index;
            if (count >= VITA_SVC_RUNTIME) return fail(l, "%u imports", count, 0);
            image->imports = calloc(count ? count : 1, sizeof *image->imports);
            if (!image->imports) return fail(l, "out of memory", 0, 0);
            image->import_count = count;
        }
    }
    return true;
}
static bool exports(Loader* l, uint32_t top, uint32_t end) {
    VitaImage* image = l->image;
    for (uint32_t p = top; p < end;) {
        const unsigned char* e = at(image, p, 0x20);
        if (!e || le16(e) < 0x20) return fail(l, "bad export table entry at 0x%08x", p, 0);
        uint32_t count = le16(e + 6) + le32(e + 8) + le32(e + 12), nids = le32(e + 24), entries = le32(e + 28);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t nid, value;
            if (!word(image, nids + i * 4, &nid) || !word(image, entries + i * 4, &value))
                return fail(l, "bad export table at 0x%08x", p, 0);
            if (nid == NID_MODULE_START) image->module_start = value;
            if (nid == NID_MODULE_STOP) image->module_stop = value;
            if (nid == NID_PROCESS_PARAM) image->process_param = value;
        }
        p += le16(e);
    }
    return true;
}
static bool identity(VitaImage* image) {
    static const char domain[] = "ScarletNX Vita image identity v1";
    EVP_MD_CTX* hash = EVP_MD_CTX_new();
    unsigned length = 0;
    bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 &&
              EVP_DigestUpdate(hash, domain, sizeof domain) == 1;
    for (unsigned i = 0; ok && i < image->segment_count; ++i) {
        unsigned char header[12];
        put32(header, image->segment[i].address), put32(header + 4, image->segment[i].size);
        put32(header + 8, image->segment[i].flags);
        ok = EVP_DigestUpdate(hash, header, sizeof header) == 1 &&
             EVP_DigestUpdate(hash, image->segment[i].bytes, image->segment[i].size) == 1;
    }
    ok = ok && EVP_DigestFinal_ex(hash, image->identity, &length) == 1 && length == sizeof image->identity;
    EVP_MD_CTX_free(hash);
    return ok;
}
/* The program headers and their bytes, from a SELF's segment table (each maybe deflated) or the ELF's own */
static bool programs(Loader* l, const unsigned char* file, size_t size, Program* out, unsigned* count,
                     const unsigned char** elf) {
    bool self = size >= 0x78 && le32(file) == 0x00454353u; /* "SCE\0" */
    uint64_t elf_offset = self ? le64(file + 0x40) : 0, phdr_offset, info = self ? le64(file + 0x58) : 0;
    if (elf_offset > size || size - elf_offset < 52 || memcmp(file + elf_offset, "\x7f" "ELF\x01\x01", 6))
        return fail(l, "eboot.bin is neither a SELF nor an ELF", 0, 0);
    *elf = file + elf_offset;
    if (le16(*elf + 18) != 40) return fail(l, "eboot.bin is not an ARM module (machine %u)", le16(*elf + 18), 0);
    *count = le16(*elf + 44);
    phdr_offset = self ? le64(file + 0x48) : le32(*elf + 28);
    if (*count > 16 || phdr_offset > size || (size - phdr_offset) / 32 < *count ||
        (self && (info > size || (size - info) / 32 < *count)))
        return fail(l, "bad program headers (%u)", *count, 0);
    for (unsigned i = 0; i < *count; ++i) {
        const unsigned char* ph = file + phdr_offset + i * 32;
        Program* p = &out[i];
        p->type = le32(ph), p->vaddr = le32(ph + 8), p->filesz = le32(ph + 16), p->memsz = le32(ph + 20);
        p->flags = le32(ph + 24), p->align = le32(ph + 28);
        uint64_t offset = le32(ph + 4), length = p->filesz;
        unsigned compression = 1;
        if (self) {
            const unsigned char* s = file + info + i * 32;
            offset = le64(s), length = le64(s + 8), compression = (unsigned)le64(s + 16);
            if (le64(s + 24) != 2 && length)
                return fail(l, "eboot.bin is encrypted (segment %u): a retail game's needs decrypting first", i, 0);
        }
        if (offset > size || length > size - offset) return fail(l, "segment %u is outside the file", i, 0);
        if (compression == 2) {
            p->owned = malloc(p->filesz ? p->filesz : 1);
            if (!p->owned || !vita_zlib_inflate(file + offset, (size_t)length, p->owned, p->filesz))
                return fail(l, "cannot decompress segment %u", i, 0);
            p->data = p->owned, p->size = p->filesz;
        } else {
            if (length < p->filesz && p->type == PT_LOAD) return fail(l, "segment %u is truncated", i, 0);
            p->data = file + offset, p->size = (size_t)length;
        }
    }
    return true;
}
bool vita_image_load(const char* path, VitaImage* image, char* error, size_t error_size) {
    memset(image, 0, sizeof *image);
    Loader l = {.image = image, .error = error, .error_size = error_size};
    for (unsigned i = 0; i < 16; ++i) l.loaded[i] = -1;
    size_t size = 0;
    unsigned char* file = snx_read_file(path, &size);
    Program ph[16] = {{0}};
    unsigned count = 0;
    const unsigned char* elf = NULL;
    bool ok = file != NULL;
    if (!ok) {
        char why[256];
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
    }
    ok = ok && programs(&l, file, size, ph, &count, &elf);
    uint32_t type = ok ? le16(elf + 16) : 0, entry = ok ? le32(elf + 24) : 0;
    if (ok && type != ET_SCE_RELEXEC)
        ok = fail(&l, type == ET_SCE_EXEC ? "eboot.bin is a fixed-address module (ET_SCE_EXEC), not supported yet"
                                          : "eboot.bin is not a Vita module (ELF type 0x%x)",
                  type, 0);
    /* the loaded segments keep their places relative to each other, the first at VITA_LOAD_BASE */
    uint32_t first = UINT32_MAX;
    for (unsigned i = 0; ok && i < count; ++i)
        if (ph[i].type == PT_LOAD && ph[i].vaddr < first) first = ph[i].vaddr & ~(PAGE - 1);
    for (unsigned i = 0; ok && i < count; ++i) {
        if (ph[i].type != PT_LOAD) continue;
        if (image->segment_count == VITA_SEGMENTS || ph[i].memsz < ph[i].filesz || !ph[i].memsz ||
            ph[i].vaddr - first > 0x10000000u || ph[i].memsz > 0x10000000u) {
            ok = fail(&l, "bad loadable segment %u", i, 0);
            break;
        }
        VitaSegment* s = &image->segment[image->segment_count];
        s->address = VITA_LOAD_BASE + (ph[i].vaddr - first);
        s->size = ph[i].memsz;
        s->flags = ph[i].flags;
        s->bytes = calloc(1, s->size);
        if (!s->bytes) {
            ok = fail(&l, "out of memory", 0, 0);
            break;
        }
        memcpy(s->bytes, ph[i].data, ph[i].filesz);
        for (unsigned j = 0; j < image->segment_count; ++j)
            if (s->address < image->segment[j].address + image->segment[j].size &&
                image->segment[j].address < s->address + s->size)
                ok = fail(&l, "segments %u and %u overlap", j, image->segment_count);
        l.base[i] = s->address;
        l.loaded[i] = (int)image->segment_count++;
    }
    for (unsigned i = 0; ok && i < count; ++i)
        if (ph[i].type == PT_SCE_RELA) ok = relocate_all(&l, ph[i].data, ph[i].size);
    /* the module info, in the segment e_entry's top bits name, at the offset its others give */
    unsigned info_segment = entry >> 30;
    const unsigned char* info = NULL;
    if (ok && (info_segment >= count || l.loaded[info_segment] < 0 ||
               !(info = at(image, l.base[info_segment] + (entry & 0x3fffffffu), 0x5c))))
        ok = fail(&l, "no module info (e_entry 0x%08x)", entry, 0);
    if (ok) {
        uint32_t base = l.base[info_segment];
        memcpy(image->name, info + 4, 27);
        ok = exports(&l, base + le32(info + 36), base + le32(info + 40)) &&
             imports(&l, base + le32(info + 44), base + le32(info + 48));
        if (ok && !image->module_start && le32(info + 68)) image->module_start = base + le32(info + 68);
        if (ok && !image->module_start) ok = fail(&l, "the module has no module_start", 0, 0);
        if (ok && le32(info + 64)) ok = fail(&l, "the module has thread-local data (%u bytes), not supported yet",
                                             le32(info + 64), 0);
    }
    if (ok) {
        seed(&l, image->module_start);
        if (image->module_stop) seed(&l, image->module_stop);
        ok = identity(image) || fail(&l, "cannot hash the image", 0, 0);
    }
    for (unsigned i = 0; i < count; ++i) free(ph[i].owned);
    free(file);
    if (!ok) vita_image_free(image);
    return ok;
}
void vita_image_free(VitaImage* image) {
    for (unsigned i = 0; i < image->segment_count; ++i) free(image->segment[i].bytes);
    free(image->imports);
    free(image->seeds);
    memset(image, 0, sizeof *image);
}
