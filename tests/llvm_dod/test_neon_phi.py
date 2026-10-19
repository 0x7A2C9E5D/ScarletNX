#!/usr/bin/env python3
"""JIT-check NEON SSA merge and register writeback for scalar/D/Q lanes."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
s=(root/'src/common/cpu/recompiler/llvm/ir_neon_memory.inc').read_text()
a=s.index('static void write_lanes(')
b=s.index('{',a);depth=1;p=b+1
while depth:
    if s[p]=='{':depth+=1
    if s[p]=='}':depth-=1
    p+=1
write=s[a:p]
fp=(root/'src/common/cpu/recompiler/llvm/ir_simd.c').read_text()
assert fp.count('LLVMBuildPhi(B, LLVMTypeOf(va)')==2
assert 'copy_of(' not in fp and 'assign(' not in fp
text=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *LLVMValueRef,*LLVMContextRef,*LLVMModuleRef,*LLVMBuilderRef,*LLVMTypeRef,*LLVMBasicBlockRef,*LLVMExecutionEngineRef;
typedef LLVMValueRef V;
typedef struct {LLVMContextRef ctx;LLVMModuleRef mod;LLVMBuilderRef b;LLVMTypeRef t_i8,t_i64,t_i128;V c;} SnxIr;
enum {LLVMVectorTypeKind=13,LLVMIntNE=33,LLVMReturnStatusAction=2};
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt8TypeInContext(LLVMContextRef),LLVMInt64TypeInContext(LLVMContextRef),LLVMInt128TypeInContext(LLVMContextRef);
extern LLVMTypeRef LLVMIntTypeInContext(LLVMContextRef,unsigned),LLVMVectorType(LLVMTypeRef,unsigned),LLVMPointerTypeInContext(LLVMContextRef,unsigned);
extern LLVMTypeRef LLVMFunctionType(LLVMTypeRef,LLVMTypeRef*,unsigned,int),LLVMVoidTypeInContext(LLVMContextRef);
extern LLVMTypeRef LLVMTypeOf(V),LLVMGetElementType(LLVMTypeRef);
extern unsigned LLVMGetVectorSize(LLVMTypeRef),LLVMGetIntTypeWidth(LLVMTypeRef);
extern int LLVMGetTypeKind(LLVMTypeRef);
extern V LLVMConstInt(LLVMTypeRef,uint64_t,int),LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMGetParam(V,unsigned);
extern V LLVMBuildLoad2(LLVMBuilderRef,LLVMTypeRef,V,const char*),LLVMBuildStore(LLVMBuilderRef,V,V);
extern V LLVMBuildGEP2(LLVMBuilderRef,LLVMTypeRef,V,V*,unsigned,const char*);
extern V LLVMBuildAdd(LLVMBuilderRef,V,V,const char*),LLVMBuildTrunc(LLVMBuilderRef,V,LLVMTypeRef,const char*);
extern V LLVMBuildZExt(LLVMBuilderRef,V,LLVMTypeRef,const char*),LLVMBuildBitCast(LLVMBuilderRef,V,LLVMTypeRef,const char*);
extern V LLVMBuildLShr(LLVMBuilderRef,V,V,const char*),LLVMBuildICmp(LLVMBuilderRef,int,V,V,const char*);
extern V LLVMBuildExtractElement(LLVMBuilderRef,V,V,const char*),LLVMBuildInsertElement(LLVMBuilderRef,V,V,V,const char*);
extern V LLVMBuildPhi(LLVMBuilderRef,LLVMTypeRef,const char*),LLVMGetUndef(LLVMTypeRef);
extern void LLVMAddIncoming(V,V*,LLVMBasicBlockRef*,unsigned);
extern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef,V,const char*),LLVMGetInsertBlock(LLVMBuilderRef);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,LLVMBasicBlockRef),LLVMSetAlignment(V,unsigned);
extern V LLVMBuildCondBr(LLVMBuilderRef,V,LLVMBasicBlockRef,LLVMBasicBlockRef),LLVMBuildBr(LLVMBuilderRef,LLVMBasicBlockRef);
extern V LLVMBuildRetVoid(LLVMBuilderRef);
extern int LLVMVerifyModule(LLVMModuleRef,int,char**);
extern char *LLVMPrintModuleToString(LLVMModuleRef);
extern void LLVMDisposeMessage(char*);
extern void LLVMLinkInMCJIT(void);
extern int LLVMCreateExecutionEngineForModule(LLVMExecutionEngineRef*,LLVMModuleRef,char**);
extern uint64_t LLVMGetFunctionAddress(LLVMExecutionEngineRef,const char*);
extern void LLVMDisposeExecutionEngine(LLVMExecutionEngineRef);
extern void LLVMInitializeX86TargetInfo(void),LLVMInitializeX86Target(void),LLVMInitializeX86TargetMC(void),LLVMInitializeX86AsmPrinter(void);
#define B ir->b
static V ir_i32(SnxIr*ir,uint32_t v){return LLVMConstInt(LLVMIntTypeInContext(ir->ctx,32),v,0);}
static V ir_i64(SnxIr*ir,uint64_t v){return LLVMConstInt(ir->t_i64,v,0);}
static void ir_set_vd(SnxIr*ir,unsigned rd,unsigned lane,V value){
    V offset=ir_i64(ir,rd*16+lane*8);
    V ptr=LLVMBuildGEP2(B,ir->t_i8,ir->c,&offset,1,"");
    V store=LLVMBuildStore(B,value,ptr);LLVMSetAlignment(store,1);
}
''' + write + r'''
static void make(LLVMModuleRef module,LLVMContextRef ctx,LLVMBuilderRef builder,unsigned width,unsigned n,unsigned q){
    SnxIr ir={.ctx=ctx,.mod=module,.b=builder,.t_i8=LLVMInt8TypeInContext(ctx),
               .t_i64=LLVMInt64TypeInContext(ctx),.t_i128=LLVMInt128TypeInContext(ctx)};
    LLVMTypeRef ptr=LLVMPointerTypeInContext(ctx,0),t32=LLVMIntTypeInContext(ctx,32);
    LLVMTypeRef args[]={ptr,ptr,ptr,t32};
    char name[64];snprintf(name,sizeof(name),"phi_%u_%u_%u",width,n,q);
    V fn=LLVMAddFunction(module,name,LLVMFunctionType(LLVMVoidTypeInContext(ctx),args,4,0));
    LLVMPositionBuilderAtEnd(builder,LLVMAppendBasicBlockInContext(ctx,fn,"entry"));
    ir.c=LLVMGetParam(fn,0);
    LLVMTypeRef lane=LLVMIntTypeInContext(ctx,width),vt=n==1?lane:LLVMVectorType(lane,n);
    V va=LLVMBuildLoad2(builder,vt,LLVMGetParam(fn,1),""),vb=LLVMBuildLoad2(builder,vt,LLVMGetParam(fn,2),"");
    LLVMSetAlignment(va,1);LLVMSetAlignment(vb,1);
    V fast=LLVMBuildAdd(builder,va,vb,"");
    LLVMBasicBlockRef yes=LLVMAppendBasicBlockInContext(ctx,fn,"fast");
    LLVMBasicBlockRef no=LLVMAppendBasicBlockInContext(ctx,fn,"slow");
    LLVMBasicBlockRef join=LLVMAppendBasicBlockInContext(ctx,fn,"join");
    LLVMBuildCondBr(builder,LLVMBuildICmp(builder,LLVMIntNE,LLVMGetParam(fn,3),ir_i32(&ir,0),""),yes,no);
    LLVMPositionBuilderAtEnd(builder,yes);LLVMBasicBlockRef fast_end=LLVMGetInsertBlock(builder);
    LLVMBuildBr(builder,join);
    LLVMPositionBuilderAtEnd(builder,no);
    V result=n==1?NULL:LLVMGetUndef(vt);
    for(unsigned i=0;i<n;++i){
        V x=n==1?va:LLVMBuildExtractElement(builder,va,ir_i32(&ir,i),"");
        V y=n==1?vb:LLVMBuildExtractElement(builder,vb,ir_i32(&ir,i),"");
        V value=LLVMBuildAdd(builder,x,y,"");
        result=n==1?value:LLVMBuildInsertElement(builder,result,value,ir_i32(&ir,i),"");
    }
    LLVMBasicBlockRef slow_end=LLVMGetInsertBlock(builder);LLVMBuildBr(builder,join);
    LLVMPositionBuilderAtEnd(builder,join);
    V merged=LLVMBuildPhi(builder,vt,"neon_result");
    V values[]={fast,result};LLVMBasicBlockRef from[]={fast_end,slow_end};
    LLVMAddIncoming(merged,values,from,2);
    write_lanes(&ir,0,merged,q);
    LLVMBuildRetVoid(builder);
}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();LLVMLinkInMCJIT();
    LLVMContextRef ctx=LLVMContextCreate();LLVMModuleRef module=LLVMModuleCreateWithNameInContext("neon_phi",ctx);
    LLVMBuilderRef builder=LLVMCreateBuilderInContext(ctx);
    unsigned widths[]={32,32,64,32};unsigned ns[]={1,2,2,4};unsigned qs[]={0,0,1,1};
    for(unsigned i=0;i<4;++i)make(module,ctx,builder,widths[i],ns[i],qs[i]);
    char*error=NULL;
    if(LLVMVerifyModule(module,LLVMReturnStatusAction,&error)){fprintf(stderr,"invalid phi: %s\n",error);return 1;}
    LLVMExecutionEngineRef engine=NULL;
    if(LLVMCreateExecutionEngineForModule(&engine,module,&error)){fprintf(stderr,"JIT: %s\n",error);return 2;}
    uint32_t seed=0xa5a5a5a5;
    for(unsigned i=0;i<4;++i){
        char name[64];sprintf(name,"phi_%u_%u_%u",widths[i],ns[i],qs[i]);
        void(*fn)(void*,void*,void*,unsigned)=(void(*)(void*,void*,void*,unsigned))(uintptr_t)LLVMGetFunctionAddress(engine,name);
        if(!fn)return 3;
        for(unsigned k=0;k<2000;++k){
            uint8_t a[16],b[16],expected[16]={0};
            for(unsigned j=0;j<16;++j){seed=1664525u*seed+1013904223u;a[j]=(uint8_t)(seed>>24);seed=1664525u*seed+1013904223u;b[j]=(uint8_t)(seed>>24);}
            for(unsigned j=0;j<ns[i];++j){
                uint64_t x=0,y=0;unsigned bytes=widths[i]/8;
                memcpy(&x,a+j*bytes,bytes);memcpy(&y,b+j*bytes,bytes);
                uint64_t sum=x+y;memcpy(expected+j*bytes,&sum,bytes);
            }
            for(unsigned choice=0;choice<2;++choice){
                uint8_t dest[16];memset(dest,0xab,16);
                fn(dest,a,b,choice);
                if(memcmp(dest,expected,16)){fprintf(stderr,"phi mismatch %s case %u path %u\n",name,k,choice);return 4;}
                /* register aliasing: source and destination share the same memory */
                memcpy(dest,a,16);fn(dest,dest,b,choice);
                if(memcmp(dest,expected,16)){fprintf(stderr,"alias mismatch %s case %u path %u\n",name,k,choice);return 5;}
            }
        }
    }
    LLVMDisposeExecutionEngine(engine);
    puts("PASS NEON SSA merge: scalar/D/Q, 16000 fast/slow cases and aliasing, JIT-executed LLVM IR");
    return 0;
}
'''
Path(sys.argv[1]).write_text(text)
