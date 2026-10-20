/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Switch dumps through hactool (hactool/snx_hac.h): the keys from their files into hactool's keyset in memory, the
   title keys of the dump's tickets, and the NCAs a game is made from */
#include "nx/runtime/driver/container.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *value;
    size_t name_size, value_size;
} KeyLine;

static char* text_copy(const char* text, size_t size) {
    char* copy;
    if (size == (size_t)-1) return NULL;
    copy = (char*)malloc(size + 1);
    if (copy) {
        memcpy(copy, text, size);
        copy[size] = 0;
    }
    return copy;
}

static int ascii_name(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int ascii_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

static char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int has_suffix(const char* text, const char* suffix) {
    size_t a, b, i;
    a = strlen(text);
    b = strlen(suffix);
    if (a < b) return 0;
    for (i = 0; i < b; i++)
        if (ascii_lower(text[a - b + i]) != ascii_lower(suffix[i])) return 0;
    return 1;
}

static SnxU64 le_bytes(const unsigned char* p, unsigned n) {
    SnxU64 value = 0;
    while (n--) value = value << 8 | p[n];
    return value;
}

/* -1 when path can't be read, else 0 for an NSP, 1 for an XCI, 2 for neither. A gamecard dump can start with the
   card's 0x1000-byte key area, its header after it (LibHac's XciHeader) */
static int kind_of(const char* path) {
    unsigned char header[0x1104];
    size_t size;
    FILE* file;
    int failed;
    file = snx_fopen(path, "rb");
    if (!file) return -1;
    size = fread(header, 1, sizeof header, file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return -1;
    if (size >= 4 && !memcmp(header, "PFS0", 4)) return 0;
    if ((size >= 0x104 && (!memcmp(header + 0x100, "HEAD", 4) || !memcmp(header + 0x100, "DXCI", 4))) ||
        (size == sizeof header && (!memcmp(header + 0x1100, "HEAD", 4) || !memcmp(header + 0x1100, "DXCI", 4))))
        return 1;
    return 2;
}

int snx_container_kind(const char* path, int* xci) {
    char why[256];
    int kind = kind_of(path);
    if (kind < 0) return snx_error("Cannot read %s: %s", path, snx_os_error(why, sizeof why));
    if (kind == 2) return snx_error("%s is not an NSP/DNSP (PFS0) or an XCI/DXCI (HEAD/DXCI)", path);
    *xci = kind;
    return 0;
}

int snx_is_dump(const char* path) {
    int kind = kind_of(path);
    return kind == 0 || kind == 1;
}

char* snx_find_keys(const char* given, const char* name, const char* repo) {
    const char* home;
    char *in_repo, *directory, *in_home, *absolute;
    if (given) return snx_absolute(given);
    in_repo = repo ? snx_path_join(repo, name) : NULL;
    if (in_repo && snx_is_file(in_repo)) return in_repo;
#if defined(_WIN32)
    home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    home = getenv("HOME");
#endif
    if (!home || !home[0]) return in_repo ? in_repo : snx_strdup(name);
    directory = snx_path_join(home, ".switch");
    in_home = directory ? snx_path_join(directory, name) : NULL;
    free(directory);
    if (!in_home) {
        free(in_repo);
        return NULL;
    }
    if (!snx_is_file(in_home)) {
        if (!in_repo) return in_home;
        free(in_home);
        return in_repo;
    }
    absolute = snx_absolute(in_home);
    free(in_home);
    free(in_repo);
    return absolute;
}

static int key_line(const char* text, size_t size, KeyLine* line) {
    size_t i, start;
    i = 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_name((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->name = text + start;
    line->name_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    if (i == size || text[i++] != '=') return 0;
    while (i < size && isspace((unsigned char)text[i])) i++;
    start = i;
    while (i < size && ascii_hex((unsigned char)text[i])) i++;
    if (i == start) return 0;
    line->value = text + start;
    line->value_size = i - start;
    while (i < size && isspace((unsigned char)text[i])) i++;
    return i == size;
}

static void secret_free(void* memory, size_t size) {
    volatile unsigned char* p;
    p = (volatile unsigned char*)memory;
    if (!p) return;
    while (size) {
        *p++ = 0;
        size--;
    }
    free(memory);
}

int snx_hac_error(const char* what) {
    const char* said = snx_hac_message();
    return said[0] ? snx_error("%s (hactool: %s)", what, said) : snx_error("%s", what);
}

static int hex_bytes(const char* text, size_t size, unsigned char* out) {
    size_t i;
    unsigned digit;
    for (i = 0; i < size; i++) {
        if (!ascii_hex((unsigned char)text[i])) return -1;
        digit = text[i] <= '9' ? (unsigned)(text[i] - '0') : (unsigned)(ascii_lower(text[i]) - 'a' + 10);
        if (i & 1)
            out[i / 2] = (unsigned char)(out[i / 2] | digit);
        else
            out[i / 2] = (unsigned char)(digit << 4);
    }
    return 0;
}

/* prod.keys into hactool's keyset. A value of a length no key has is left out, as hactool would stop on it */
static int load_prod_keys(SnxDump* dump, int say) {
    unsigned char* data;
    size_t size, start, end;
    unsigned long dropped;
    char *name, *value;
    KeyLine line;
    int failed;
    char why[256];
    data = snx_read_file(dump->keys, &size);
    if (!data) return snx_error("Cannot read %s: %s", dump->keys, snx_os_error(why, sizeof why));
    start = 0;
    dropped = 0;
    failed = 0;
    while (start < size && !failed) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line)) {
            if (line.value_size % 32)
                dropped++;
            else {
                name = text_copy(line.name, line.name_size);
                value = text_copy(line.value, line.value_size);
                if (!name || !value)
                    failed = snx_error("Out of memory reading prod.keys");
                else
                    snx_hac_set_key(dump->hac, name, value);
                if (name) secret_free(name, line.name_size);
                if (value) secret_free(value, line.value_size);
            }
        }
        start = end < size ? end + 1 : end;
    }
    secret_free(data, size);
    if (failed) return 1;
    snx_hac_derive_keys(dump->hac);
    if (dropped && say) printf("Left out %lu malformed key lines\n", dropped);
    return 0;
}

/* title.keys, when there is one: a rights ID and its title key a line */
static int load_title_keys(SnxDump* dump) {
    unsigned char *data, rights[16], key[16];
    char why[256];
    size_t size, start, end;
    KeyLine line;
    if (!snx_is_file(dump->title_keys)) return 0;
    data = snx_read_file(dump->title_keys, &size);
    if (!data) return snx_error("Cannot read %s: %s", dump->title_keys, snx_os_error(why, sizeof why));
    start = 0;
    while (start < size) {
        end = start;
        while (end < size && data[end] != '\n') end++;
        if (key_line((const char*)data + start, end - start, &line) && line.name_size == 32 && line.value_size == 32 &&
            !hex_bytes(line.name, 32, rights) && !hex_bytes(line.value, 32, key))
            snx_hac_add_title_key(dump->hac, rights, key);
        start = end < size ? end + 1 : end;
    }
    memset(key, 0, sizeof key);
    secret_free(data, size);
    return 0;
}

/* the title keys of a container's tickets: common tickets, whose key is stored as is */
static int read_tickets(SnxDump* dump, int container) {
    unsigned char data[1024];
    const SnxHacEntry* entries;
    size_t count, i, size, offset;
    count = snx_hac_entries(dump->hac, container, &entries);
    for (i = 0; i < count; i++) {
        if (!has_suffix(entries[i].name, ".tik")) continue;
        size = entries[i].size < sizeof data ? (size_t)entries[i].size : sizeof data;
        if (snx_hac_read(dump->hac, container, i, data, size))
            return snx_error("Cannot read the ticket %s", entries[i].name);
        if (size < 4) continue;
        switch ((SnxU32)le_bytes(data, 4)) {
            case 0x10000UL:
            case 0x10003UL: offset = 4 + 0x200 + 0x3c; break;
            case 0x10001UL:
            case 0x10004UL: offset = 4 + 0x100 + 0x3c; break;
            case 0x10002UL:
            case 0x10005UL: offset = 4 + 0x3c + 0x40; break;
            case 0x10006UL: offset = 4 + 0x14 + 0x28; break;
            default: continue;
        }
        if (size < offset + 0x170 || data[offset + 0x141]) continue; /* a personalized ticket's key is encrypted */
        snx_hac_add_title_key(dump->hac, data + offset + 0x160, data + offset + 0x40);
    }
    memset(data, 0, sizeof data);
    return 0;
}

/* 1 when an NCA's header is encrypted: a decrypted one (DNCA) or a plain NCA3 or NCA2 header needs no keys */
static int nca_needs_keys(SnxDump* dump, int container, size_t entry) {
    unsigned char header[0x204];
    if (snx_hac_read(dump->hac, container, entry, header, sizeof header)) return 1;
    return memcmp(header + 0x200, "DNCA", 4) && memcmp(header + 0x200, "NCA3", 4) && memcmp(header + 0x200, "NCA2", 4);
}

static void take(SnxNcaRef* ref, int container, size_t entry, const SnxHacEntry* e, SnxU64 title_id) {
    ref->found = 1;
    ref->container = container;
    ref->entry = entry;
    ref->size = e->size;
    ref->title_id = title_id;
}

/* the headers of a container's NCAs, the content meta's left out (SNX_HAC_OTHER), into *ncas (owned, count of them
   as there are entries): 0, else nonzero, said why. One whose header cannot be decrypted counts in *unreadable */
static int read_ncas(SnxDump* dump, int container, const SnxHacEntry** entries, size_t* count, SnxHacNca** ncas,
                     unsigned long* unreadable) {
    size_t i;
    *count = snx_hac_entries(dump->hac, container, entries);
    *ncas = (SnxHacNca*)calloc(*count ? *count : 1, sizeof **ncas);
    *unreadable = 0;
    if (!*ncas) return snx_error("Out of memory reading the NCAs' headers");
    for (i = 0; i < *count; i++) {
        (*ncas)[i].type = SNX_HAC_OTHER;
        if (!has_suffix((*entries)[i].name, ".nca") || has_suffix((*entries)[i].name, ".cnmt.nca")) continue;
        if (!dump->has_keys && nca_needs_keys(dump, container, i)) {
            free(*ncas);
            *ncas = NULL;
            return snx_error("Encrypted content needs prod.keys. Provide --keys, or put it at %s or in ~/.switch",
                             dump->keys);
        }
        if (snx_hac_nca(dump->hac, container, i, &(*ncas)[i])) {
            (*ncas)[i].type = SNX_HAC_OTHER;
            ++*unreadable;
        }
    }
    return 0;
}

/* an update's Program NCA: its title ID the game's with 0x800 added, or a Patch RomFS */
static int is_patch(const SnxHacNca* nca) {
    return nca->type == SNX_HAC_PROGRAM && (snx_addon_kind(nca->title_id) == SNX_ADDON_UPDATE || nca->patch);
}

/* the base's Program NCA of a container (selected), the patch an NSP of both has (embedded_patch), and the Control
   NCA of the same title (control). The biggest when there are several */
static int select_ncas(SnxDump* dump, int container, SnxNcaRef* selected, SnxNcaRef* embedded_patch,
                       SnxNcaRef* control) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    size_t count, i;
    unsigned long unreadable;
    const SnxU64 update_bit = 0x800UL;
    memset(selected, 0, sizeof *selected);
    if (embedded_patch) memset(embedded_patch, 0, sizeof *embedded_patch);
    memset(control, 0, sizeof *control);
    if (read_ncas(dump, container, &entries, &count, &ncas, &unreadable)) return 1;
    for (i = 0; i < count; i++)
        if (ncas[i].type == SNX_HAC_PROGRAM && !is_patch(&ncas[i]) &&
            (!selected->found || entries[i].size > selected->size))
            take(selected, container, i, &entries[i], ncas[i].title_id);
    for (i = 0; selected->found && i < count; i++) {
        if (embedded_patch && is_patch(&ncas[i]) && (ncas[i].title_id & ~update_bit) == selected->title_id &&
            (!embedded_patch->found || entries[i].size > embedded_patch->size))
            take(embedded_patch, container, i, &entries[i], ncas[i].title_id);
        if (!control->found && ncas[i].type == SNX_HAC_CONTROL && /* the same game's, base or update */
            (ncas[i].title_id & ~update_bit) == (selected->title_id & ~update_bit))
            take(control, container, i, &entries[i], ncas[i].title_id);
    }
    free(ncas);
    if (selected->found) return 0;
    if (unreadable) return snx_hac_error("The NCAs' headers cannot be decrypted. Check that prod.keys is current");
    return snx_error("Container has no base Program NCA (possibly an update or DLC). Supply a base game");
}

/* An add-on's container: an update (its Program NCA, the patch, with its Control NCA) or else a DLC (its Data NCA, a
   title ID past the game's add-on base). With the base opened (program found), only the base's own add-ons; without
   it, the first game's whose add-on the container has. The biggest when there are several */
static int select_addon(SnxDump* dump, int container) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    size_t count, i;
    unsigned long unreadable;
    SnxU64 game = dump->program.found ? dump->program.title_id : 0, own;
    int patches = 0; /* another game's */
    memset(&dump->patch, 0, sizeof dump->patch);
    memset(&dump->aoc, 0, sizeof dump->aoc);
    if (read_ncas(dump, container, &entries, &count, &ncas, &unreadable)) return 1;
    for (i = 0; i < count; i++) {
        own = snx_addon_base(ncas[i].title_id);
        patches += is_patch(&ncas[i]);
        if (is_patch(&ncas[i]) && (!game || own == game) &&
            (!dump->patch.found || (own == snx_addon_base(dump->patch.title_id) && entries[i].size > dump->patch.size)))
            take(&dump->patch, container, i, &entries[i], ncas[i].title_id);
    }
    for (i = 0; i < count; i++) {
        own = snx_addon_base(ncas[i].title_id);
        if (dump->patch.found && ncas[i].type == SNX_HAC_CONTROL && own == snx_addon_base(dump->patch.title_id))
            take(&dump->control, container, i, &entries[i], ncas[i].title_id); /* the update's, a name changed in it */
        else if (!dump->patch.found && ncas[i].type == SNX_HAC_DATA &&
                 snx_addon_kind(ncas[i].title_id) == SNX_ADDON_DLC && (!game || own == game) &&
                 (!dump->aoc.found || (ncas[i].title_id == dump->aoc.title_id && entries[i].size > dump->aoc.size)))
            take(&dump->aoc, container, i, &entries[i], ncas[i].title_id);
    }
    free(ncas);
    if (dump->patch.found || dump->aoc.found) return 0;
    if (unreadable) return snx_hac_error("The NCAs' headers cannot be decrypted. Check that prod.keys is current");
    if (game && patches) return snx_error("Update has no Program NCA matching the selected base game");
    return snx_error(game ? "The DLC is not the selected base game's" : "Container has no update or DLC");
}

static int open_container(SnxDump* dump, const char* path, int say) {
    int xci, container;
    if (snx_container_kind(path, &xci)) return -1;
    if (say) {
        printf("Reading %s\n", path);
        fflush(stdout);
    }
    container = snx_hac_open(dump->hac, path, xci);
    if (container < 0) snx_hac_error(xci ? "Cannot read the XCI's secure partition" : "Cannot read the NSP");
    return container < 0 || read_tickets(dump, container) ? -1 : container;
}

/* the keys loaded into a new hactool: 0, else nonzero, said why */
static int dump_start(SnxDump* dump, const char* keys, const char* title_keys, const char* repo, int say) {
    memset(dump, 0, sizeof *dump);
    dump->base = dump->update = -1;
    dump->keys = snx_find_keys(keys, "prod.keys", repo);
    dump->title_keys = snx_find_keys(title_keys, "title.keys", repo);
    dump->hac = snx_hac_new();
    if (!dump->keys || !dump->title_keys || !dump->hac) return snx_error("Out of memory reading the dump");
    dump->has_keys = snx_is_file(dump->keys);
    if (keys && !dump->has_keys) return snx_error("%s is not a file", dump->keys);
    return (dump->has_keys && load_prod_keys(dump, say)) || load_title_keys(dump);
}

int snx_dump_open(SnxDump* dump, const char* base, const char* addon, const char* keys, const char* title_keys,
                  const char* repo, int say) {
    if (dump_start(dump, keys, title_keys, repo, say)) return 1;
    if (base && (dump->base = open_container(dump, base, say)) < 0) return 1;
    if (addon && (dump->update = open_container(dump, addon, say)) < 0) return 1;
    if (base && select_ncas(dump, dump->base, &dump->program, addon ? NULL : &dump->patch, &dump->control)) return 1;
    return addon ? select_addon(dump, dump->update) : 0;
}

int snx_dump_title(SnxDump* dump, const char* path, const char* keys, const char* title_keys, const char* repo, int say,
                   SnxU64* title) {
    const SnxHacEntry* entries;
    SnxHacNca* ncas;
    size_t count, i;
    unsigned long unreadable;
    int game = 0;
    if (dump_start(dump, keys, title_keys, repo, say) || (dump->base = open_container(dump, path, say)) < 0 ||
        read_ncas(dump, dump->base, &entries, &count, &ncas, &unreadable))
        return 1;
    for (i = 0; i < count && !game; i++) game = ncas[i].type == SNX_HAC_PROGRAM && !is_patch(&ncas[i]);
    free(ncas);
    if (!game) {
        dump->update = dump->base;
        dump->base = -1;
        if (select_addon(dump, dump->update)) return 1;
        *title = dump->patch.found ? dump->patch.title_id : dump->aoc.title_id;
        return 0;
    }
    if (select_ncas(dump, dump->base, &dump->program, &dump->patch, &dump->control)) return 1;
    *title = dump->program.title_id;
    return 0;
}

void snx_dump_close(SnxDump* dump) {
    snx_hac_free(dump->hac);
    free(dump->keys);
    free(dump->title_keys);
    memset(dump, 0, sizeof *dump);
}

SnxHacFs* snx_dump_fs(SnxDump* dump) {
    const SnxNcaRef* top = dump->patch.found ? &dump->patch : &dump->program;
    int under = dump->patch.found && dump->program.found;
    SnxHacFs* fs = snx_hac_fs_open(dump->hac, top->container, top->entry, under ? dump->program.container : -1,
                                   dump->program.entry);
    if (!fs) snx_hac_error("The Program NCA cannot be read");
    return fs;
}

SnxHacFs* snx_dump_aoc_fs(SnxDump* dump) {
    SnxHacFs* fs = snx_hac_fs_open(dump->hac, dump->aoc.container, dump->aoc.entry, -1, 0);
    if (!fs) snx_hac_error("The DLC's Data NCA cannot be read");
    return fs;
}

/* The RomFS's header and tables (Switchbrew's "RomFS"): ten little-endian 64-bit fields, the root's directory entry
   first in its table, a file entry its parent, sibling, offset and size, then its name */
unsigned char* snx_romfs_file(SnxHacFs* fs, const char* name, size_t* size) {
    unsigned char header[0x50], entry[0x20 + 0x300], *data;
    SnxU64 total = snx_hac_fs_romfs_size(fs), table, table_size, files, start, length;
    SnxU32 at, name_size;
    size_t want = strlen(name);
    unsigned visited;
    if (total < sizeof header || snx_hac_fs_read_romfs(fs, 0, header, sizeof header)) return NULL;
    table = le_bytes(header + 0x38, 8);
    table_size = le_bytes(header + 0x40, 8);
    files = le_bytes(header + 0x48, 8);
    if (le_bytes(header + 0x18, 8) > total - 0x10 ||
        snx_hac_fs_read_romfs(fs, le_bytes(header + 0x18, 8), entry, 0x10)) /* the root's first file */
        return NULL;
    at = (SnxU32)le_bytes(entry + 0xc, 4);
    for (visited = 0; at != 0xFFFFFFFFUL && visited < 0x100000; visited++) {
        if (table > total || table_size > total - table || at > table_size || 0x20 > table_size - at ||
            snx_hac_fs_read_romfs(fs, table + at, entry, 0x20))
            return NULL;
        name_size = (SnxU32)le_bytes(entry + 0x1c, 4);
        if (name_size == want && name_size <= 0x300 && 0x20 + (SnxU64)name_size <= table_size - at &&
            !snx_hac_fs_read_romfs(fs, table + at + 0x20, entry + 0x20, name_size) &&
            !memcmp(entry + 0x20, name, want)) {
            start = le_bytes(entry + 8, 8);
            length = le_bytes(entry + 0x10, 8);
            if (files > total || start > total - files || length > total - files - start || length > 0x1000000UL)
                return NULL;
            data = (unsigned char*)malloc(length ? (size_t)length : 1);
            if (data && snx_hac_fs_read_romfs(fs, files + start, data, (size_t)length)) {
                free(data);
                data = NULL;
            }
            if (data) *size = (size_t)length;
            return data;
        }
        at = (SnxU32)le_bytes(entry + 4, 4);
    }
    return NULL;
}

unsigned char* snx_dump_nacp(SnxDump* dump, size_t* size, const char** why) {
    SnxHacFs* fs;
    unsigned char* nacp;
    *why = NULL;
    if (!dump->control.found) {
        *why = "it has no Control NCA";
        return NULL;
    }
    fs = snx_hac_fs_open(dump->hac, dump->control.container, dump->control.entry, -1, 0);
    if (!fs) {
        *why = "its Control NCA cannot be read";
        return NULL;
    }
    nacp = snx_romfs_file(fs, "control.nacp", size);
    snx_hac_fs_free(fs);
    if (!nacp) *why = "its Control NCA holds no control.nacp";
    return nacp;
}
