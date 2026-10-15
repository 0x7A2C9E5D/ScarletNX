/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend runs without the machine outliner (snx_ir_init): on AArch64 it is a module pass that keeps every
   function of a unit in memory until it runs. minsize functions with one body are what it outlines, so an AArch64
   object built from them after snx_ir_init must name no OUTLINED_FUNCTION. Skipped (77) without an AArch64 code
   generator: the host's, or the one a shared libLLVM carries beside it. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "ir.h"
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

#define TRIPLE "aarch64-unknown-linux-gnu"
#define SKIP   77

static int aarch64(LLVMTargetRef* target) {
    char* message = NULL;
    int missing = LLVMGetTargetFromTriple(TRIPLE, target, &message);
    LLVMDisposeMessage(message);
#ifndef _WIN32
    if (missing) {
        static const char* const names[] = {"LLVMInitializeAArch64TargetInfo", "LLVMInitializeAArch64Target",
                                            "LLVMInitializeAArch64TargetMC", "LLVMInitializeAArch64AsmPrinter"};
        size_t i;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            union {
                void* p;
                void (*f)(void);
            } init;
            init.p = dlsym(RTLD_DEFAULT, names[i]);
            if (!init.p) return 0;
            init.f();
        }
        message = NULL;
        missing = LLVMGetTargetFromTriple(TRIPLE, target, &message);
        LLVMDisposeMessage(message);
    }
#endif
    return !missing;
}

/* four minsize functions, each 24 volatile stores of the same constants */
static LLVMModuleRef same_bodies(LLVMContextRef ctx) {
    static const char* const attributes[] = {"minsize", "optsize", "nounwind"};
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("outliner", ctx);
    LLVMTypeRef ptr = LLVMPointerTypeInContext(ctx, 0), i32 = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef type = LLVMFunctionType(LLVMVoidTypeInContext(ctx), &ptr, 1, 0);
    LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
    char name[8];
    int f, k;
    size_t a;
    LLVMSetTarget(mod, TRIPLE);
    for (f = 0; f < 4; ++f) {
        LLVMValueRef fn;
        sprintf(name, "f%d", f);
        fn = LLVMAddFunction(mod, name, type);
        for (a = 0; a < sizeof(attributes) / sizeof(attributes[0]); ++a) {
            unsigned kind = LLVMGetEnumAttributeKindForName(attributes[a], strlen(attributes[a]));
            LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ctx, kind, 0));
        }
        LLVMPositionBuilderAtEnd(b, LLVMAppendBasicBlockInContext(ctx, fn, "entry"));
        for (k = 0; k < 24; ++k) {
            LLVMValueRef index = LLVMConstInt(LLVMInt64TypeInContext(ctx), (unsigned long long)k, 0);
            LLVMValueRef at = LLVMBuildInBoundsGEP2(b, i32, LLVMGetParam(fn, 0), &index, 1, "");
            LLVMSetVolatile(LLVMBuildStore(b, LLVMConstInt(i32, (unsigned long long)(1000 + 7 * k), 0), at), 1);
        }
        LLVMBuildRetVoid(b);
    }
    LLVMDisposeBuilder(b);
    return mod;
}

static int contains(const char* data, size_t size, const char* text) {
    size_t i, n = strlen(text);
    for (i = 0; i + n <= size; ++i)
        if (!memcmp(data + i, text, n)) return 1;
    return 0;
}

int main(void) {
    char error[256], *message = NULL;
    LLVMTargetRef target;
    LLVMTargetMachineRef tm;
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMMemoryBufferRef object;
    int outlined;
    if (snx_ir_init(error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (!aarch64(&target)) {
        puts("skipped: this LLVM has no AArch64 code generator");
        return SKIP;
    }
    tm = LLVMCreateTargetMachine(target, TRIPLE, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
                                 LLVMCodeModelDefault);
    ctx = LLVMContextCreate();
    mod = same_bodies(ctx);
    if (LLVMTargetMachineEmitToMemoryBuffer(tm, mod, LLVMObjectFile, &message, &object)) {
        fprintf(stderr, "AArch64 object: %s\n", message ? message : "failed");
        return 1;
    }
    outlined = contains(LLVMGetBufferStart(object), LLVMGetBufferSize(object), "OUTLINED_FUNCTION");
    LLVMDisposeMemoryBuffer(object);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    LLVMDisposeTargetMachine(tm);
    if (outlined) {
        fprintf(stderr, "the AArch64 object has outlined functions: the machine outliner runs\n");
        return 1;
    }
    puts("no machine outliner: minsize functions with one body stay whole on AArch64");
    return 0;
}
