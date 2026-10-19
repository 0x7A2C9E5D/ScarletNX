#!/usr/bin/env python3
"""Emit real fast ADD/SUB IR and execute its IXC-dependent TwoSum branches."""
from pathlib import Path
import sys
s=(Path(__file__).resolve().parents[2]/'src/common/cpu/recompiler/llvm/ir_fp.c').read_text()
a=s.index('typedef struct {',s.index('/* ---- fast paths ---- */'))
b=s.index('LLVMBasicBlockRef ir_fp_branch(',a)
body=s[a:b]
code=r'''
#include "common/cpu/aarch64/cpu.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef uint64_t SnxU64;
typedef void *V,*LLVMValueRef,*LLVMContextRef,*LLVMModuleRef,*LLVMBuilderRef,*LLVMTypeRef,*LLVMBasicBlockRef,*LLVMExecutionEngineRef;
typedef struct { int x86, fma; } SnxIrFeatures;
typedef struct { LLVMBasicBlockRef block; V nearest, ixc; } SnxIrFpGuards;
typedef struct { V value,ok,inexact; } SnxFpFast;
typedef struct { LLVMContextRef ctx;LLVMModuleRef mod;LLVMBuilderRef b;LLVMTypeRef t_i1,t_i8,t_i32,t_i64;V fn,c;const SnxIrFeatures*features;SnxIrFpGuards fp_guards; } SnxIr;
enum {SNX_IR_FP_ADD,SNX_IR_FP_SUB,SNX_IR_FP_MUL,SNX_IR_FP_DIV,SNX_IR_FP_SQRT,SNX_IR_FP_FMA};
enum {LLVMRealUNE=14,LLVMIntEQ=32,LLVMIntNE=33,LLVMIntULE=37,LLVMIntSGE=39};
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt1TypeInContext(LLVMContextRef),LLVMInt8TypeInContext(LLVMContextRef),LLVMInt32TypeInContext(LLVMContextRef),LLVMInt64TypeInContext(LLVMContextRef),LLVMFloatTypeInContext(LLVMContextRef),LLVMDoubleTypeInContext(LLVMContextRef),LLVMVectorType(LLVMTypeRef,unsigned),LLVMFunctionType(LLVMTypeRef,LLVMTypeRef*,unsigned,int),LLVMGetElementType(LLVMTypeRef),LLVMTypeOf(V);
extern V LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMGetParam(V,unsigned),LLVMConstInt(LLVMTypeRef,uint64_t,int),LLVMConstReal(LLVMTypeRef,double),LLVMConstVector(V*,unsigned);
extern V LLVMBuildFAdd(LLVMBuilderRef,V,V,const char*),LLVMBuildFSub(LLVMBuilderRef,V,V,const char*),LLVMBuildFMul(LLVMBuilderRef,V,V,const char*),LLVMBuildFDiv(LLVMBuilderRef,V,V,const char*),LLVMBuildFNeg(LLVMBuilderRef,V,const char*);
extern V LLVMBuildAdd(LLVMBuilderRef,V,V,const char*),LLVMBuildSub(LLVMBuilderRef,V,V,const char*),LLVMBuildAnd(LLVMBuilderRef,V,V,const char*),LLVMBuildOr(LLVMBuilderRef,V,V,const char*),LLVMBuildXor(LLVMBuilderRef,V,V,const char*),LLVMBuildNot(LLVMBuilderRef,V,const char*),LLVMBuildLShr(LLVMBuilderRef,V,V,const char*),LLVMBuildZExt(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildTrunc(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildBitCast(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildFCmp(LLVMBuilderRef,int,V,V,const char*),LLVMBuildICmp(LLVMBuilderRef,int,V,V,const char*),LLVMBuildSelect(LLVMBuilderRef,V,V,V,const char*),LLVMBuildLoad2(LLVMBuilderRef,LLVMTypeRef,V,const char*),LLVMBuildStore(LLVMBuilderRef,V,V),LLVMBuildInBoundsGEP2(LLVMBuilderRef,LLVMTypeRef,V,V*,unsigned,const char*);
extern V LLVMBuildPhi(LLVMBuilderRef,LLVMTypeRef,const char*),LLVMBuildCondBr(LLVMBuilderRef,V,LLVMBasicBlockRef,LLVMBasicBlockRef),LLVMBuildBr(LLVMBuilderRef,LLVMBasicBlockRef),LLVMBuildRetVoid(LLVMBuilderRef);
extern void LLVMAddIncoming(V,V*,LLVMBasicBlockRef*,unsigned);
extern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef,V,const char*),LLVMGetInsertBlock(LLVMBuilderRef);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,LLVMBasicBlockRef);
extern const char* LLVMGetTarget(LLVMModuleRef);
extern unsigned LLVMLookupIntrinsicID(const char*,size_t);
extern V LLVMGetIntrinsicDeclaration(LLVMModuleRef,unsigned,LLVMTypeRef*,size_t),LLVMBuildCall2(LLVMBuilderRef,LLVMTypeRef,V,V*,unsigned,const char*);
extern LLVMTypeRef LLVMIntrinsicGetType(LLVMContextRef,unsigned,LLVMTypeRef*,size_t);
extern void LLVMLinkInMCJIT(void),LLVMInitializeX86TargetInfo(void),LLVMInitializeX86Target(void),LLVMInitializeX86TargetMC(void),LLVMInitializeX86AsmPrinter(void);
extern int LLVMVerifyModule(LLVMModuleRef,int,char**),LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef*,LLVMModuleRef,char**);
extern uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef,const char*);
#define B ir->b
static V i32c(SnxIr*ir,uint32_t v){return LLVMConstInt(ir->t_i32,v,0);}
static size_t ir_off_fpcr(void){return offsetof(CPU,fpcr);}
static V ir_cpu_load(SnxIr*ir,LLVMTypeRef type,size_t off){V index=LLVMConstInt(ir->t_i64,off,0);return LLVMBuildLoad2(ir->b,type,LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&index,1,""),"");}
static V ir_intrinsic(SnxIr*ir,const char* name,LLVMTypeRef type,V* args,unsigned count){unsigned id=LLVMLookupIntrinsicID(name,strlen(name));V fn=LLVMGetIntrinsicDeclaration(ir->mod,id,&type,1);return LLVMBuildCall2(ir->b,LLVMIntrinsicGetType(ir->ctx,id,&type,1),fn,args,count,"");}
static LLVMBasicBlockRef ir_block(SnxIr*ir,const char*name){return LLVMAppendBasicBlockInContext(ir->ctx,ir->fn,name);}
''' + body +r'''
static void build(SnxIr*ir,unsigned width,unsigned op){
    char name[50];sprintf(name,"fp_%u_%u",width,op);
    LLVMTypeRef t=width==32?ir->t_i32:ir->t_i64;
    LLVMTypeRef types[]={LLVMInt64TypeInContext(ir->ctx),t,t};
    types[0]=LLVMInt64TypeInContext(ir->ctx); /* host CPU pointer passed as i64, then inttoptr */
    LLVMTypeRef ptr=LLVMInt8TypeInContext(ir->ctx);(void)ptr;
    /* LLVM opaque pointer param is precisely the guest CPU*. */
    extern LLVMTypeRef LLVMPointerTypeInContext(LLVMContextRef,unsigned);
    types[0]=LLVMPointerTypeInContext(ir->ctx,0);
    ir->fn=LLVMAddFunction(ir->mod,name,LLVMFunctionType(LLVMInt8TypeInContext(ir->ctx),types,3,0));
    LLVMPositionBuilderAtEnd(ir->b,LLVMAppendBasicBlockInContext(ir->ctx,ir->fn,"entry"));
    ir->c=LLVMGetParam(ir->fn,0); ir_fp_guards_invalidate(ir);
    SnxFpFast f={0};
    ir_fp_fast(ir,op,width,1,LLVMGetParam(ir->fn,1),LLVMGetParam(ir->fn,2),NULL,&f);
    V offset=LLVMConstInt(ir->t_i64,offsetof(CPU,x),0);
    V address=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&offset,1,"");
    LLVMBuildStore(ir->b,width==32?LLVMBuildZExt(ir->b,f.value,ir->t_i64,""):f.value,address);
    offset=LLVMConstInt(ir->t_i64,offsetof(CPU,x)+8,0);
    address=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&offset,1,"");
    LLVMBuildStore(ir->b,LLVMBuildZExt(ir->b,f.ok,ir->t_i64,""),address);
    offset=LLVMConstInt(ir->t_i64,offsetof(CPU,x)+16,0);
    address=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&offset,1,"");
    LLVMBuildStore(ir->b,LLVMBuildZExt(ir->b,f.inexact,ir->t_i64,""),address);
    extern V LLVMBuildRet(LLVMBuilderRef,V);
    LLVMBuildRet(ir->b,LLVMConstInt(ir->t_i8,0,0));
}
static uint64_t f64bits(double x){uint64_t bits;memcpy(&bits,&x,8);return bits;}
static uint32_t f32bits(float x){uint32_t bits;memcpy(&bits,&x,4);return bits;}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();LLVMLinkInMCJIT();
    SnxIrFeatures features={.x86=1,.fma=1};
    SnxIr ir={0};ir.ctx=LLVMContextCreate();ir.mod=LLVMModuleCreateWithNameInContext("precise_guard",ir.ctx);ir.b=LLVMCreateBuilderInContext(ir.ctx);
    ir.t_i1=LLVMInt1TypeInContext(ir.ctx);ir.t_i8=LLVMInt8TypeInContext(ir.ctx);ir.t_i32=LLVMInt32TypeInContext(ir.ctx);ir.t_i64=LLVMInt64TypeInContext(ir.ctx);ir.features=&features;
    for(unsigned width=32;width<=64;width+=32)for(unsigned op=0;op<2;++op)build(&ir,width,op);
    char*err=NULL;
    if(LLVMVerifyModule(ir.mod,2,&err)){fprintf(stderr,"invalid FP IR: %s\n",err);return 2;}
    LLVMExecutionEngineRef jit=NULL;
    if(LLVMCreateExecutionEngineForModule(&jit,ir.mod,&err)){fprintf(stderr,"LLVM JIT: %s\n",err);return 3;}
    uint64_t cases=0;
    for(unsigned width=32;width<=64;width+=32)for(unsigned op=0;op<2;++op){
        char name[50];sprintf(name,"fp_%u_%u",width,op);
        void(*fn32)(CPU*,uint32_t,uint32_t)=(void*)(uintptr_t)LLVMGetFunctionAddress(jit,name);
        void(*fn64)(CPU*,uint64_t,uint64_t)=(void*)fn32;
        if(!fn32)return 4;
        for(unsigned ixc=0;ixc<2;++ixc)for(unsigned rm=0;rm<4;++rm)for(unsigned sample=0;sample<1000;++sample){
            CPU cpu={0};cpu.fpcr=rm<<22;cpu.fpsr=ixc<<4;
            if(width==32){
                float a=sample&1?1.0f:1.5f, b=sample&1?0x1p-24f:2.25f;
                if(sample%7==0){a=2.0f;b=1.0f;}
                fn32(&cpu,f32bits(a),f32bits(b));
                float expected=op==SNX_IR_FP_ADD?a+b:a-b;
                if((uint32_t)cpu.x[0]!=f32bits(expected)){fprintf(stderr,"f32 result mismatch\n");return 5;}
                if(rm==0 && cpu.x[1]!=1){fprintf(stderr,"f32 fast guard mismatch\n");return 6;}
            }else{
                double a=sample&1?1.0:1.5, b=sample&1?0x1p-53:2.25;
                if(sample%7==0){a=2.0;b=1.0;}
                fn64(&cpu,f64bits(a),f64bits(b));
                double expected=op==SNX_IR_FP_ADD?a+b:a-b;
                if(cpu.x[0]!=f64bits(expected)){fprintf(stderr,"f64 result mismatch\n");return 7;}
                if(rm==0 && cpu.x[1]!=1){fprintf(stderr,"f64 fast guard mismatch\n");return 8;}
            }
            if(ixc && cpu.x[2]!=0){fprintf(stderr,"sticky IXC residual should be skipped\n");return 9;}
            if(rm && cpu.x[1]!=0){fprintf(stderr,"non-nearest FP mode reached fast path\n");return 10;}
            ++cases;
        }
    }
    printf("PASS emitted FP TwoSum: %llu native-JIT cases, sticky/clear IXC and all 4 rounding settings\n",(unsigned long long)cases);
    return 0;
}
'''
Path(sys.argv[1]).write_text(code)
