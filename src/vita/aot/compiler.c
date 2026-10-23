/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The offline compiler, part of the driver: the image's blocks to a native module, as the 3DS driver's is
   (3ds/aot/compiler.c). The blocks are found by following the code from what is known to be code: module_start, the
   code pointers the relocations wrote, and the hints of earlier runs. Each block is compiled once into a scratch
   module to learn where the block compiler ends it, which says where the next ones are (cpu/arm/successors.h); the
   workers then compile them for real, in chunks, in parallel */
#include "vita/aot/module.h"
#include "vita/aot/runtime.h"
#include "common/cpu/arm/llvm.h"
#include "common/cpu/arm/successors.h"
#include "common/utilities/common.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define CHUNK_BLOCKS 512u

typedef struct {
    uint32_t key, units;
} Block;
typedef struct {
    const VitaImage* image;
    Block* blocks;
    size_t count, capacity;
    uint32_t* set; /* every key ever queued: open addressing, 0 empty (no code is at address 0) */
    size_t set_count, set_capacity;
    uint32_t* work;
    size_t work_count, work_capacity;
    bool failed;
} Discovery;
typedef struct {
    char* triple;
    LLVMTargetMachineRef machine;
    LLVMTargetDataRef layout;
} Host;

static uint32_t little(const unsigned char* p, unsigned size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < size; ++i) result |= (uint32_t)p[i] << (i * 8);
    return result;
}
static void block_name(char out[24], uint32_t key) {
    snprintf(out, 24, "v_%c_%08x", key & 1 ? 't' : 'a', (unsigned)(key & ~1u));
}
/* the code from address to the end of its executable segment, and how many units of it (at most the block limit) */
static const unsigned char* code_at(const VitaImage* image, uint32_t address, int thumb, size_t* units) {
    const VitaSegment* s = vita_image_segment(image, address, thumb ? 2 : 4);
    *units = 0;
    if (!s || !(s->flags & 1)) return NULL;
    *units = (s->address + s->size - address) / (thumb ? 2 : 4);
    if (*units > VITA_BLOCK_LIMIT) *units = VITA_BLOCK_LIMIT;
    return s->bytes + (address - s->address);
}
static bool insert(Discovery* d, uint32_t key) {
    if ((d->set_count + 1) * 2 > d->set_capacity) {
        size_t capacity = d->set_capacity ? d->set_capacity * 2 : 4096;
        uint32_t* set = calloc(capacity, sizeof *set);
        if (!set) {
            d->failed = true;
            return false;
        }
        for (size_t j = 0; j < d->set_capacity; ++j)
            if (d->set[j]) {
                size_t i = (size_t)(d->set[j] * 0x9E3779B1u) & (capacity - 1);
                while (set[i]) i = (i + 1) & (capacity - 1);
                set[i] = d->set[j];
            }
        free(d->set);
        d->set = set, d->set_capacity = capacity;
    }
    size_t i = (size_t)(key * 0x9E3779B1u) & (d->set_capacity - 1);
    while (d->set[i]) {
        if (d->set[i] == key) return false;
        i = (i + 1) & (d->set_capacity - 1);
    }
    d->set[i] = key;
    ++d->set_count;
    return true;
}
static void queue(void* context, uint32_t address, int thumb) {
    Discovery* d = context;
    size_t units;
    if (address & (thumb ? 1u : 3u) || !code_at(d->image, address, thumb, &units) || !units) return;
    uint32_t key = address | (thumb ? 1u : 0u);
    if (!insert(d, key)) return;
    if (d->work_count == d->work_capacity) {
        size_t capacity = d->work_capacity ? d->work_capacity * 2 : 1024;
        uint32_t* work = realloc(d->work, capacity * sizeof *work);
        if (!work) {
            d->failed = true;
            return;
        }
        d->work = work, d->work_capacity = capacity;
    }
    d->work[d->work_count++] = key;
}
static void queue_pointer(Discovery* d, uint32_t pointer) { queue(d, pointer & ~1u, (int)(pointer & 1)); }
/* the block compiler's input: the guest's little-endian code as host integers, the caller's to free */
static void* units_of(const unsigned char* code, size_t count, int thumb) {
    if (thumb) {
        uint16_t* h = malloc(count * sizeof *h);
        for (size_t i = 0; h && i < count; ++i) h[i] = (uint16_t)little(code + i * 2, 2);
        return h;
    }
    uint32_t* w = malloc(count * sizeof *w);
    for (size_t i = 0; w && i < count; ++i) w[i] = little(code + i * 4, 4);
    return w;
}
static LLVMValueRef emit(LLVMModuleRef module, const VitaImage* image, uint32_t key, size_t count, const char* name,
                         size_t* consumed) {
    size_t available;
    int thumb = (int)(key & 1);
    const unsigned char* code = code_at(image, key & ~1u, thumb, &available);
    if (!code || count > available) return NULL;
    void* input = units_of(code, count, thumb);
    if (!input) return NULL;
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block(module, name, input, count, key & ~1u, consumed)
                            : snx_arm_llvm_block(module, name, input, count, key & ~1u, consumed);
    free(input);
    return fn;
}
static bool discover(Discovery* d, const char* folder, char* error, size_t error_size) {
    const VitaImage* image = d->image;
    size_t hint_count = 0, refused = 0;
    uint32_t* hints = vita_hints_read(folder, &hint_count);
    if (hint_count) printf("Seeding discovery with %zu blocks earlier runs compiled at run time\n", hint_count);
    queue_pointer(d, image->module_start);
    if (image->module_stop) queue_pointer(d, image->module_stop);
    for (size_t i = 0; i < image->seed_count; ++i) queue_pointer(d, image->seeds[i]);
    for (size_t i = 0; i < hint_count; ++i) queue_pointer(d, hints[i]);
    free(hints);
    LLVMContextRef context = LLVMContextCreate();
    LLVMModuleRef scratch = LLVMModuleCreateWithNameInContext("vita-discovery", context);
    while (d->work_count && !d->failed) {
        uint32_t key = d->work[--d->work_count];
        size_t count, consumed = 0;
        const unsigned char* code = code_at(image, key & ~1u, (int)(key & 1), &count);
        LLVMValueRef fn = emit(scratch, image, key, count, "block", &consumed);
        if (!fn || !consumed) { /* not code after all (data a pointer led to): the run-time compiler has it if not */
            ++refused;
            continue;
        }
        LLVMDeleteFunction(fn);
        if (d->count == d->capacity) {
            size_t capacity = d->capacity ? d->capacity * 2 : 4096;
            Block* grown = realloc(d->blocks, capacity * sizeof *grown);
            if (!grown) {
                d->failed = true;
                break;
            }
            d->blocks = grown, d->capacity = capacity;
        }
        d->blocks[d->count++] = (Block){key, (uint32_t)consumed};
        snx_arm_successors(key & ~1u, code, consumed, count, (int)(key & 1), queue, d);
    }
    LLVMDisposeModule(scratch);
    LLVMContextDispose(context);
    if (d->failed) {
        snprintf(error, error_size, "out of memory discovering the code");
        return false;
    }
    printf("Discovered %zu blocks (%zu addresses were not code)\n", d->count, refused);
    return true;
}
static int by_key(const void* a, const void* b) {
    uint32_t x = ((const Block*)a)->key, y = ((const Block*)b)->key;
    return (x > y) - (x < y);
}

static bool host_new(Host* h, char* error, size_t size) {
    LLVMTargetRef backend;
    char* detail = NULL;
    h->triple = LLVMGetDefaultTargetTriple();
    if (LLVMGetTargetFromTriple(h->triple, &backend, &detail)) {
        snprintf(error, size, "%s", detail);
        LLVMDisposeMessage(detail);
        return false;
    }
    h->machine = LLVMCreateTargetMachine(backend, h->triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                         LLVMCodeModelDefault);
    if (!h->machine) {
        snprintf(error, size, "cannot create an LLVM target machine for %s", h->triple);
        return false;
    }
    h->layout = LLVMCreateTargetDataLayout(h->machine);
    return true;
}
static void host_free(Host* h) {
    if (h->layout) LLVMDisposeTargetData(h->layout);
    if (h->machine) LLVMDisposeTargetMachine(h->machine);
    LLVMDisposeMessage(h->triple);
    memset(h, 0, sizeof *h);
}
static LLVMModuleRef new_module(Host* h, LLVMContextRef c) {
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext("vita-aot", c);
    LLVMSetTarget(m, h->triple);
    LLVMSetModuleDataLayout(m, h->layout);
    return m;
}
static bool write_object(Host* h, LLVMModuleRef mod, const char* output, const char* pipeline, char* error,
                         size_t size) {
    char* detail = NULL;
    if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    detail = NULL;
    if (pipeline) {
        LLVMPassBuilderOptionsRef passes = LLVMCreatePassBuilderOptions();
        LLVMErrorRef failure = LLVMRunPasses(mod, pipeline, h->machine, passes);
        LLVMDisposePassBuilderOptions(passes);
        if (failure) {
            char* message = LLVMGetErrorMessage(failure);
            snprintf(error, size, "%s", message);
            LLVMDisposeErrorMessage(message);
            return false;
        }
    }
    if (LLVMTargetMachineEmitToFile(h->machine, mod, (char*)output, LLVMObjectFile, &detail)) goto fail;
    LLVMDisposeMessage(detail);
    return true;
fail:
    snprintf(error, size, "%s", detail ? detail : "cannot emit a native object");
    LLVMDisposeMessage(detail);
    return false;
}

/* the chunks shared out to a worker per host core, each with a target machine and LLVM contexts of its own */
typedef struct {
    const VitaImage* image;
    const Block* blocks;
    char** objects; /* chunk i's object */
    size_t count;
    unsigned chunks, next, finished;
    bool failed;
    char error[512];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Work;
static void work_lock(Work* w) {
#ifdef _WIN32
    EnterCriticalSection(&w->lock);
#else
    pthread_mutex_lock(&w->lock);
#endif
}
static void work_unlock(Work* w) {
#ifdef _WIN32
    LeaveCriticalSection(&w->lock);
#else
    pthread_mutex_unlock(&w->lock);
#endif
}
static bool chunk(Host* h, const Work* w, unsigned index, char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    size_t begin = (size_t)index * CHUNK_BLOCKS, end = begin + CHUNK_BLOCKS;
    if (end > w->count) end = w->count;
    bool ok = true;
    for (size_t i = begin; ok && i < end; ++i) {
        char name[24];
        size_t consumed = 0;
        block_name(name, w->blocks[i].key);
        LLVMValueRef fn = emit(mod, w->image, w->blocks[i].key, w->blocks[i].units, name, &consumed);
        if (!fn || consumed != w->blocks[i].units) {
            snprintf(error, size, "the block at 0x%08x compiled differently from its discovery",
                     (unsigned)w->blocks[i].key);
            ok = false;
            break;
        }
        LLVMSetVisibility(fn, LLVMHiddenVisibility);
    }
    ok = ok && write_object(h, mod, w->objects[index], "default<O1>", error, size);
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
static void worker(Work* w) {
    Host h = {0};
    char error[512] = "";
    bool ok = host_new(&h, error, sizeof error);
    for (;;) {
        work_lock(w);
        if (!ok && !w->failed) {
            w->failed = true;
            snprintf(w->error, sizeof w->error, "%s", error);
        }
        unsigned i = w->failed ? w->chunks : w->next++;
        work_unlock(w);
        if (i >= w->chunks) break;
        ok = chunk(&h, w, i, error, sizeof error);
        work_lock(w);
        if (ok && (!(++w->finished % 16) || w->finished == w->chunks))
            printf("Compiled %u/%u chunks of blocks\n", w->finished, w->chunks);
        work_unlock(w);
    }
    host_free(&h);
}
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID w) {
    worker(w);
    return 0;
}
#else
static void* worker_main(void* w) {
    worker(w);
    return NULL;
}
#endif
static bool compile_chunks(Work* w) {
    unsigned jobs = snx_cpu_count(), created = 0;
    if (jobs > w->chunks) jobs = w->chunks;
    if (jobs < 1) jobs = 1;
#ifdef _WIN32
    HANDLE* threads = calloc(jobs, sizeof *threads);
    InitializeCriticalSection(&w->lock);
#else
    pthread_t* threads = calloc(jobs, sizeof *threads);
    pthread_mutex_init(&w->lock, NULL);
#endif
    for (unsigned i = 1; threads && i < jobs; ++i) {
#ifdef _WIN32
        if (!(threads[created] = CreateThread(NULL, 0, worker_main, w, 0, NULL))) break;
#else
        if (pthread_create(&threads[created], NULL, worker_main, w)) break;
#endif
        ++created;
    }
    worker(w);
    for (unsigned i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&w->lock);
#else
    pthread_mutex_destroy(&w->lock);
#endif
    free(threads);
    return !w->failed;
}
/* The index: the sorted keys, each one's block, and the descriptor (VitaModule) the runtime looks up */
static bool index_object(Host* h, const VitaImage* image, const Block* blocks, size_t count, const char* output,
                         char* error, size_t size) {
    LLVMContextRef c = LLVMContextCreate();
    LLVMModuleRef mod = new_module(h, c);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(c), i32 = LLVMInt32TypeInContext(c), i64 = LLVMInt64TypeInContext(c);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(c, 0), ft = LLVMFunctionType(LLVMVoidTypeInContext(c), &ptr, 1, 0);
    size_t n = count ? count : 1;
    LLVMValueRef *keys = malloc(n * sizeof *keys), *functions = malloc(n * sizeof *functions);
    bool ok = keys && functions;
    for (size_t i = 0; ok && i < n; ++i) {
        char name[24];
        keys[i] = LLVMConstInt(i32, i < count ? blocks[i].key : 0, 0);
        if (i < count) {
            block_name(name, blocks[i].key);
            functions[i] = LLVMAddFunction(mod, name, ft);
            LLVMSetVisibility(functions[i], LLVMHiddenVisibility);
        } else
            functions[i] = LLVMConstNull(ptr);
    }
    if (ok) {
        LLVMValueRef tables[2];
        LLVMTypeRef types[2] = {LLVMArrayType(i32, (unsigned)n), LLVMArrayType(ptr, (unsigned)n)};
        LLVMValueRef values[2] = {LLVMConstArray(i32, keys, (unsigned)n), LLVMConstArray(ptr, functions, (unsigned)n)};
        for (unsigned t = 0; t < 2; ++t) {
            tables[t] = LLVMAddGlobal(mod, types[t], t ? "vita_blocks" : "vita_keys");
            LLVMSetInitializer(tables[t], values[t]);
            LLVMSetGlobalConstant(tables[t], 1);
            LLVMSetLinkage(tables[t], LLVMInternalLinkage);
        }
        LLVMValueRef hash[32], fields[10];
        LLVMTypeRef field_types[10] = {i64, i32, i32, i32, i32, i32, i32, LLVMArrayType(i8, 32), ptr, ptr};
        for (unsigned i = 0; i < 32; ++i) hash[i] = LLVMConstInt(i8, image->identity[i], 0);
        uint32_t integers[6] = {VITA_MODULE_ABI, sizeof(VitaModule), sizeof(SnxArmCPU), SNX_ARM_ABI, sizeof(void*),
                                (uint32_t)count};
        fields[0] = LLVMConstInt(i64, VITA_MODULE_MAGIC, 0);
        for (unsigned i = 0; i < 6; ++i) fields[i + 1] = LLVMConstInt(i32, integers[i], 0);
        fields[7] = LLVMConstArray(i8, hash, 32);
        fields[8] = tables[0], fields[9] = tables[1];
        LLVMTypeRef structure = LLVMStructTypeInContext(c, field_types, 10, 0);
        static const size_t offsets[10] = {offsetof(VitaModule, magic),       offsetof(VitaModule, abi),
                                           offsetof(VitaModule, descriptor_size), offsetof(VitaModule, cpu_size),
                                           offsetof(VitaModule, arm_abi),     offsetof(VitaModule, pointer_size),
                                           offsetof(VitaModule, count),       offsetof(VitaModule, identity),
                                           offsetof(VitaModule, keys),        offsetof(VitaModule, blocks)};
        ok = LLVMABISizeOfType(h->layout, structure) == sizeof(VitaModule);
        for (unsigned i = 0; ok && i < 10; ++i) ok = LLVMOffsetOfElement(h->layout, structure, i) == offsets[i];
        if (!ok)
            snprintf(error, size, "the module descriptor's layout differs between the compiler and the LLVM target");
        else {
            LLVMValueRef global = LLVMAddGlobal(mod, structure, "snx_vita_module_v1");
            LLVMSetInitializer(global, LLVMConstNamedStruct(structure, fields, 10));
            LLVMSetGlobalConstant(global, 1);
#ifdef _WIN32
            /* the only symbol the loader looks up, exported so that nothing else is (3ds/aot/compiler.c says why) */
            LLVMSetDLLStorageClass(global, LLVMDLLExportStorageClass);
#endif
            ok = write_object(h, mod, output, NULL, error, size);
        }
    } else
        snprintf(error, size, "out of memory for the module's index");
    free(keys);
    free(functions);
    LLVMDisposeModule(mod);
    LLVMContextDispose(c);
    return ok;
}
static const char* leaf(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') last = p + 1;
    return last;
}
int vita_aot_compile(const char* folder, const VitaImage* image, char* error, size_t error_size) {
    Discovery d = {.image = image};
    Host host = {0};
    Work work = {0};
    char *stage = NULL, *module = NULL, *runtime = NULL, *target = NULL, **objects = NULL;
    const char** command = NULL;
    unsigned files = 0;
    int result = 1;
    if (error_size) error[0] = 0;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        snprintf(error, error_size, "cannot initialize the native LLVM target");
        goto done;
    }
    if (!discover(&d, folder, error, error_size)) goto done;
    qsort(d.blocks, d.count, sizeof *d.blocks, by_key);
    if (!host_new(&host, error, error_size)) goto done;
    target = vita_game_file(folder, VITA_MODULE_SUFFIX);
    stage = snx_temp_dir(folder);
    if (!target || !stage) {
        char why[256];
        snprintf(error, error_size, "cannot create a staging folder in %s: %s", folder, snx_os_error(why, sizeof why));
        goto done;
    }
    unsigned chunks = (unsigned)((d.count + CHUNK_BLOCKS - 1) / CHUNK_BLOCKS);
    files = chunks + 1;
    objects = calloc(files, sizeof *objects);
    command = calloc(files + 8, sizeof *command);
    if (!objects || !command) {
        snprintf(error, error_size, "out of memory");
        goto done;
    }
    objects[0] = snx_path_join(stage, "index.o");
    for (unsigned i = 1; i < files; ++i) objects[i] = snx_format("%s/blocks-%05u.o", stage, i);
    work = (Work){.image = image, .blocks = d.blocks, .objects = objects + 1, .count = d.count, .chunks = chunks};
    if (chunks && !compile_chunks(&work)) {
        snprintf(error, error_size, "%s", work.error);
        goto done;
    }
    if (!index_object(&host, image, d.blocks, d.count, objects[0], error, error_size)) goto done;
    runtime = snx_path_join(stage, "arm-runtime.a");
    if (snx_write_file(runtime, snx_vita_aot_runtime, snx_vita_aot_runtime_size)) {
        char why[256];
        snprintf(error, error_size, "cannot write %s: %s", runtime, snx_os_error(why, sizeof why));
        goto done;
    }
    module = snx_path_join(stage, "code.so");
    unsigned arg = 0;
    command[arg++] = SNX_VITA_HOST_CC;
    command[arg++] = "-shared";
#ifdef __APPLE__
    command[arg++] = "-Wl,-undefined,error";
#elif !defined(_WIN32)
    command[arg++] = "-Wl,-z,defs,-z,noexecstack,-z,relro,-z,now";
#else
    command[arg++] = "-Wl,--nxcompat,--dynamicbase";
#endif
    /* names alone, the linker started in the staging folder (3ds/aot/compiler.c says why) */
    for (unsigned i = 0; i < files; ++i) command[arg++] = leaf(objects[i]);
    command[arg++] = leaf(runtime);
    command[arg++] = "-o";
    command[arg++] = leaf(module);
    if (snx_run_stage(command, stage, "native linking") || !snx_is_file(module) || !snx_file_size(module)) {
        snprintf(error, error_size, "the native linker failed, so no module was published");
        goto done;
    }
    if (snx_replace(module, target)) {
        char why[256];
        snprintf(error, error_size, "cannot move %s to %s: %s", module, target, snx_os_error(why, sizeof why));
        goto done;
    }
    printf("Compiled %zu blocks into %s\n", d.count, target);
    result = 0;
done:
    if (stage) snx_remove_tree(stage);
    if (objects)
        for (unsigned i = 0; i < files; ++i) free(objects[i]);
    free(objects);
    free(command);
    free(stage);
    free(module);
    free(runtime);
    free(target);
    free(d.blocks);
    free(d.set);
    free(d.work);
    host_free(&host);
    return result;
}
