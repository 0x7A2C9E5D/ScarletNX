#!/usr/bin/env python3
"""Exercise the production ABI 9 table-call emitter using the real CPU/RtAPI layouts."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[2]
emit = (root / 'src/common/cpu/recompiler/llvm/ir_emit.c').read_text()
gen = (root / 'src/common/cpu/recompiler/llvm/ir_codegen.c').read_text()
header = (root / 'src/common/cpu/recompiler/llvm/ir.h').read_text()

def extract(src, prefix):
    i = src.index(prefix)
    start = src.index('{', i)
    depth = 1
    end = start + 1
    while depth:
        if src[end] == '{': depth += 1
        if src[end] == '}': depth -= 1
        end += 1
    return src[i:end]

assert '#define SNX_ABI 9' in (root / 'src/common/cpu/aarch64/cpu.h').read_text()
assert 'size_t ir_rt_svcs(void)' in header
assert 'LLVMValueRef ir_rt_table_call(' in header
svc = extract(gen, 'static void svc(SnxIr* ir,')
assert re.search(r'codegen->direct_svc\s*&&\s*n\s*<\s*SNX_SVC_COUNT', svc)
assert 'ir_rt_table_call(ir, ir_rt_svcs(), n,' in svc
assert 'ir_rt_call(ir, ir_rt_svc(),' in svc
functions = '\n'.join(extract(emit, prefix) for prefix in (
    'static LLVMValueRef rt_pointer(SnxIr* ir)',
    'size_t ir_rt_svcs(void)',
    'LLVMValueRef ir_rt_table_call(SnxIr* ir,',
))
preamble = r'''
#include "common/cpu/aarch64/cpu.h"
#include "common/runtime/interface.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint64_t SnxU64;
typedef void* P;
typedef P LLVMValueRef;
typedef P LLVMContextRef;
typedef P LLVMTypeRef;
typedef P LLVMBuilderRef;
typedef struct { P b, c, t_ptr, t_i8, t_i32, t_i64; unsigned invalidated; } SnxIr;
extern P LLVMContextCreate(void);
extern P LLVMModuleCreateWithNameInContext(const char*,P);
extern P LLVMCreateBuilderInContext(P);
extern P LLVMInt8TypeInContext(P);
extern P LLVMInt32TypeInContext(P);
extern P LLVMInt64TypeInContext(P);
extern P LLVMPointerTypeInContext(P,unsigned);
extern P LLVMVoidTypeInContext(P);
extern P LLVMFunctionType(P,P*,unsigned,int);
extern P LLVMAddFunction(P,const char*,P);
extern P LLVMAppendBasicBlockInContext(P,P,const char*);
extern P LLVMGetParam(P,unsigned);
extern P LLVMConstInt(P,uint64_t,int);
extern P LLVMBuildInBoundsGEP2(P,P,P,P*,unsigned,const char*);
extern P LLVMBuildLoad2(P,P,P,const char*);
extern P LLVMBuildCall2(P,P,P,P*,unsigned,const char*);
extern P LLVMBuildRetVoid(P);
extern void LLVMPositionBuilderAtEnd(P,P);
extern int LLVMVerifyModule(P,int,char**);
extern char* LLVMPrintModuleToString(P);
extern void LLVMDisposeMessage(char*);
extern void LLVMDisposeBuilder(P);
extern void LLVMContextDispose(P);
extern void LLVMDisposeModule(P);
#if defined(__x86_64__)
extern void LLVMInitializeX86TargetInfo(void);
extern void LLVMInitializeX86Target(void);
extern void LLVMInitializeX86TargetMC(void);
extern void LLVMInitializeX86AsmPrinter(void);
#elif defined(__aarch64__)
extern void LLVMInitializeAArch64TargetInfo(void);
extern void LLVMInitializeAArch64Target(void);
extern void LLVMInitializeAArch64TargetMC(void);
extern void LLVMInitializeAArch64AsmPrinter(void);
#else
#error "Native execution test supports x86-64 and AArch64"
#endif
extern void LLVMLinkInMCJIT(void);
extern int LLVMCreateExecutionEngineForModule(P*,P,char**);
extern uint64_t LLVMGetFunctionAddress(P,const char*);
extern void LLVMDisposeExecutionEngine(P);
static P ir_i64(SnxIr*ir, uint64_t n) {return LLVMConstInt(ir->t_i64,n,0);}
static P ir_cpu_load(SnxIr*ir,P type,size_t off) {
    P offset=ir_i64(ir,off);
    return LLVMBuildLoad2(ir->b,type,LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&offset,1,""),"");
}
static P ir_load(SnxIr*ir,P type,P ptr) {return LLVMBuildLoad2(ir->b,type,ptr,"");}
static void ir_state_invalidate(SnxIr*ir) {++ir->invalidated;}
'''
post = r'''
static int calls;
static void on_svc(CPU* cpu, uint32_t imm) { ++calls; cpu->x[0] += imm; }
int main(void) {
    assert(SNX_ABI == 9 && offsetof(RtAPI,svcs) == 9 * sizeof(void*));
    LLVMLinkInMCJIT();
    #if defined(__x86_64__)
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();
#else
    LLVMInitializeAArch64TargetInfo();LLVMInitializeAArch64Target();LLVMInitializeAArch64TargetMC();LLVMInitializeAArch64AsmPrinter();
#endif
    P ctx=LLVMContextCreate(),mod=LLVMModuleCreateWithNameInContext("svc_abi9",ctx);
    P b=LLVMCreateBuilderInContext(ctx);
    SnxIr ir={0};ir.b=b;ir.t_ptr=LLVMPointerTypeInContext(ctx,0);
    ir.t_i8=LLVMInt8TypeInContext(ctx);ir.t_i32=LLVMInt32TypeInContext(ctx);
    ir.t_i64=LLVMInt64TypeInContext(ctx);
    P params[1]={ir.t_ptr};
    P fn=LLVMAddFunction(mod,"invoke_svc_17",LLVMFunctionType(LLVMVoidTypeInContext(ctx),params,1,0));
    LLVMPositionBuilderAtEnd(b,LLVMAppendBasicBlockInContext(ctx,fn,"entry"));
    ir.c=LLVMGetParam(fn,0);
    P callparams[2]={ir.t_ptr,ir.t_i32};
    P callargs[2]={ir.c,LLVMConstInt(ir.t_i32,17,0)};
    ir_rt_table_call(&ir,ir_rt_svcs(),17,LLVMVoidTypeInContext(ctx),callparams,callargs,2);
    assert(ir.invalidated==1);
    LLVMBuildRetVoid(b);
    char* error=NULL;
    assert(!LLVMVerifyModule(mod,2,&error));
    char* text=LLVMPrintModuleToString(mod);
    assert(strstr(text,"getelementptr inbounds ptr") && strstr(text,"i64 17"));
    LLVMDisposeMessage(text);LLVMDisposeBuilder(b);
    P ee=NULL;
    if(LLVMCreateExecutionEngineForModule(&ee,mod,&error)){
        fprintf(stderr,"MCJIT: %s\n",error?error:"unknown");return 2;
    }
    typedef void(*Entry)(CPU*);
    Entry entry=(Entry)(uintptr_t)LLVMGetFunctionAddress(ee,"invoke_svc_17");
    assert(entry);
    void(*table[SNX_SVC_COUNT])(CPU*,uint32_t)={0};
    table[17]=on_svc;
    RtAPI api={0};api.svcs=table;
    CPU cpu={0};cpu.rt=&api;cpu.x[0]=3;
    entry(&cpu);
    assert(calls==1 && cpu.x[0]==20);
    printf("PASS ABI 9 SVC table call: actual emitted LLVM IR verified and executed; register cache invalidated\n");
    LLVMDisposeExecutionEngine(ee);LLVMContextDispose(ctx);
    return 0;
}
'''
Path(sys.argv[1]).write_text(preamble + '\n' + functions + '\n' + post)
