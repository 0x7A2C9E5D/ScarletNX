#!/usr/bin/env python3
"""Verify the production block-local FP guard reuse and invalidation with LLVM."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
s=(root/'src/common/cpu/recompiler/llvm/ir_fp.c').read_text()
def fn(prefix):
    a=s.index(prefix); opening=s.index('{',a); level=1; p=opening+1
    while level:
        if s[p]=='{':level+=1
        if s[p]=='}':level-=1
        p+=1
    return s[a:p]
functions='\n\n'.join(fn(prefix) for prefix in (
    'void ir_fp_guards_invalidate(', 'static SnxIrFpGuards* fp_guards(',
    'static V rounds_to_nearest(', 'static V ixc_set('
))
code=r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *V,*LLVMValueRef,*LLVMContextRef,*LLVMModuleRef,*LLVMBuilderRef,*LLVMTypeRef,*LLVMBasicBlockRef,*LLVMExecutionEngineRef;
typedef struct {LLVMBasicBlockRef block;V nearest,ixc;} SnxIrFpGuards;
typedef struct {LLVMContextRef ctx;LLVMModuleRef mod;LLVMBuilderRef b;V c;LLVMTypeRef t_i32,t_i64,t_i8;SnxIrFpGuards fp_guards;} SnxIr;
typedef struct {uint32_t fpcr,fpsr;} CpuFp;
enum {LLVMIntEQ=32,LLVMIntNE=33,LLVMReturnStatusAction=2};
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt8TypeInContext(LLVMContextRef),LLVMInt32TypeInContext(LLVMContextRef),LLVMInt64TypeInContext(LLVMContextRef);
extern LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef,unsigned),LLVMFunctionType(LLVMTypeRef,LLVMTypeRef*,unsigned,int);
extern V LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMGetParam(V,unsigned);
extern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef,V,const char*),LLVMGetInsertBlock(LLVMBuilderRef);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,LLVMBasicBlockRef);
extern V LLVMConstInt(LLVMTypeRef,uint64_t,int),LLVMBuildInBoundsGEP2(LLVMBuilderRef,LLVMTypeRef,V,V*,unsigned,const char*);
extern V LLVMBuildLoad2(LLVMBuilderRef,LLVMTypeRef,V,const char*),LLVMBuildStore(LLVMBuilderRef,V,V);
extern V LLVMBuildICmp(LLVMBuilderRef,int,V,V,const char*),LLVMBuildAnd(LLVMBuilderRef,V,V,const char*);
extern V LLVMBuildZExt(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildShl(LLVMBuilderRef,V,V,const char*);
extern V LLVMBuildOr(LLVMBuilderRef,V,V,const char*),LLVMBuildRet(LLVMBuilderRef,V);
extern int LLVMVerifyModule(LLVMModuleRef,int,char**);
extern char* LLVMPrintModuleToString(LLVMModuleRef);
extern void LLVMDisposeMessage(char*);
extern void LLVMLinkInMCJIT(void);
extern int LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef*,LLVMModuleRef,char**);
extern uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef,const char*);
extern void LLVMDisposeExecutionEngine(LLVMExecutionEngineRef);
extern void LLVMInitializeX86TargetInfo(void),LLVMInitializeX86Target(void),LLVMInitializeX86TargetMC(void),LLVMInitializeX86AsmPrinter(void);
#define B ir->b
static V i32c(SnxIr*ir,uint32_t value){return LLVMConstInt(ir->t_i32,value,0);}
static size_t ir_off_fpcr(void){return offsetof(CpuFp,fpcr);}
static V ir_cpu_load(SnxIr*ir,LLVMTypeRef type,size_t offset){
    V n=LLVMConstInt(ir->t_i64,offset,0);
    V p=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&n,1,"");
    return LLVMBuildLoad2(ir->b,type,p,"");
}
''' +functions +r'''
static V load_test(SnxIr*ir,const char*name,int invalidate){
    LLVMTypeRef params[]={LLVMPointerTypeInContext(ir->ctx,0)};
    V f=LLVMAddFunction(ir->mod,name,LLVMFunctionType(ir->t_i32,params,1,0));
    LLVMPositionBuilderAtEnd(ir->b,LLVMAppendBasicBlockInContext(ir->ctx,f,"entry"));
    ir->c=LLVMGetParam(f,0);
    ir_fp_guards_invalidate(ir);
    V a=rounds_to_nearest(ir),b=ixc_set(ir);
    if(rounds_to_nearest(ir)!=a || ixc_set(ir)!=b){fprintf(stderr,"guard not reused\n");exit(2);}
    V lo=LLVMBuildOr(ir->b,LLVMBuildZExt(ir->b,a,ir->t_i32,""),
                     LLVMBuildShl(ir->b,LLVMBuildZExt(ir->b,b,ir->t_i32,""),i32c(ir,1),""),"");
    if(invalidate){
        V n=LLVMConstInt(ir->t_i64,ir_off_fpcr(),0);
        V p=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&n,1,"");
        LLVMBuildStore(ir->b,i32c(ir,1u<<22),p);
        ir_fp_guards_invalidate(ir); /* exactly the ir_cpu_store invalidation contract */
    }
    V c=rounds_to_nearest(ir),d=ixc_set(ir);
    V hi=LLVMBuildOr(ir->b,LLVMBuildZExt(ir->b,c,ir->t_i32,""),
                     LLVMBuildShl(ir->b,LLVMBuildZExt(ir->b,d,ir->t_i32,""),i32c(ir,1),""),"");
    LLVMBuildRet(ir->b,LLVMBuildOr(ir->b,lo,LLVMBuildShl(ir->b,hi,i32c(ir,2),""),""));
    return f;
}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();
    LLVMLinkInMCJIT();
    SnxIr ir={0};ir.ctx=LLVMContextCreate();ir.mod=LLVMModuleCreateWithNameInContext("fp_guards",ir.ctx);
    ir.b=LLVMCreateBuilderInContext(ir.ctx);ir.t_i8=LLVMInt8TypeInContext(ir.ctx);
    ir.t_i32=LLVMInt32TypeInContext(ir.ctx);ir.t_i64=LLVMInt64TypeInContext(ir.ctx);
    load_test(&ir,"shared",0);load_test(&ir,"invalidate",1);
    char*err=NULL;
    if(LLVMVerifyModule(ir.mod,LLVMReturnStatusAction,&err)){fprintf(stderr,"invalid LLVM IR: %s\n",err);return 3;}
    char*text=LLVMPrintModuleToString(ir.mod);
    int loads=0;
    for(char*p=text;(p=strstr(p,"load i32,"));++p)++loads;
    if(loads!=6){fprintf(stderr,"expected 6 guarded state loads, got %d\n",loads);return 4;}
    LLVMDisposeMessage(text);
    LLVMExecutionEngineRef jit=NULL;
    if(LLVMCreateExecutionEngineForModule(&jit,ir.mod,&err)){fprintf(stderr,"LLVM JIT: %s\n",err);return 5;}
    uint32_t(*shared)(CpuFp*)=(void*)(uintptr_t)LLVMGetFunctionAddress(jit,"shared");
    uint32_t(*invalidate)(CpuFp*)=(void*)(uintptr_t)LLVMGetFunctionAddress(jit,"invalidate");
    for(unsigned round=0;round<4;++round)for(unsigned ix=0;ix<2;++ix){
        CpuFp state={(round<<22),(ix<<4)};
        uint32_t bits=(round==0)+(2*ix);
        if(shared(&state)!=bits*5){fprintf(stderr,"shared guard mismatch\n");return 6;}
        if(invalidate(&state)!=(bits|((2*ix)<<2))){fprintf(stderr,"invalidated guard mismatch\n");return 7;}
    }
    LLVMDisposeExecutionEngine(jit);
    puts("PASS FP guards: actual source, six IR loads, single-block reuse, write invalidation, JIT-executed 8 FP states");
    return 0;
}
'''
Path(sys.argv[1]).write_text(code)
