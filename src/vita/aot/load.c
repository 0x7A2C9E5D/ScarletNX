/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/aot/module.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include "common/filesystem/host.h"
#else
#include <dlfcn.h>
#endif

bool vita_module_compatible(const VitaModule* m, const VitaImage* image) {
    return m && image && m->magic == VITA_MODULE_MAGIC && m->abi == VITA_MODULE_ABI &&
           m->descriptor_size == sizeof *m && m->cpu_size == sizeof(SnxArmCPU) && m->arm_abi == SNX_ARM_ABI &&
           m->pointer_size == sizeof(void*) && m->keys && m->blocks &&
           !memcmp(m->identity, image->identity, sizeof m->identity);
}
SnxArmBlock vita_module_block(const VitaModule* m, uint32_t key) {
    size_t lo = 0, hi = m ? m->count : 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->keys[mid] == key) return m->blocks[mid];
        if (m->keys[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
#if defined(_WIN32)
static void* library_open(const char* path, char* error, size_t error_size) {
    wchar_t* wide = snx_host_wpath(path);
    HMODULE library = wide ? LoadLibraryW(wide) : NULL;
    if (!library) snprintf(error, error_size, "cannot load %s: error %lu", path, (unsigned long)GetLastError());
    free(wide);
    return (void*)library;
}
static void* library_symbol(void* library, const char* name) {
    return (void*)(uintptr_t)GetProcAddress((HMODULE)library, name);
}
static void library_close(void* library) { FreeLibrary((HMODULE)library); }
#else
static void* library_open(const char* path, char* error, size_t error_size) {
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) snprintf(error, error_size, "cannot load %s: %s", path, dlerror());
    return library;
}
static void* library_symbol(void* library, const char* name) { return dlsym(library, name); }
static void library_close(void* library) { dlclose(library); }
#endif
bool vita_native_open(const char* path, const VitaImage* image, VitaNative* native, char* error, size_t error_size) {
    *native = (VitaNative){0};
    native->library = library_open(path, error, error_size);
    if (!native->library) return false;
    native->module = library_symbol(native->library, "snx_vita_module_v1");
    if (!vita_module_compatible(native->module, image)) {
        snprintf(error, error_size, "%s was compiled by another build or from another eboot.bin", path);
        vita_native_close(native);
        return false;
    }
    return true;
}
void vita_native_close(VitaNative* native) {
    if (native->library) library_close(native->library);
    *native = (VitaNative){0};
}
char* vita_game_file(const char* directory, const char* suffix) {
    char* folder = snx_absolute(directory);
    if (!folder) folder = snx_strdup(directory);
    if (!folder) return NULL;
    size_t n = strlen(folder), cut;
    while (n > 1 && (folder[n - 1] == '/' || folder[n - 1] == '\\')) folder[--n] = 0;
    for (cut = n; cut && folder[cut - 1] != '/' && folder[cut - 1] != '\\'; --cut) continue;
    char* file = snx_format("%s/%s%s", folder, folder + cut, suffix);
    free(folder);
    return file;
}
