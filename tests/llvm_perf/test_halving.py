#!/usr/bin/env python3
"""JIT the production NEON halving emitter, exercising every 32-bit-lane variant."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
s=(root/'src/common/cpu/recompiler/llvm/ir_neon_arithmetic.inc').read_text()
a=s.index('static V halving(');start=s.index('{',a);j=start+1;depth=1
while depth:
 if s[j]=='{':depth+=1
 if s[j]=='}':depth-=1
 j+=1
body=s[a:j]
prelude=(root/'tests/llvm_hotpaths/llvm_neon_real.c').read_text().split('static V broadcast(')[0]
prelude=prelude.replace('typedef LLVMValueRef V;','typedef LLVMValueRef V;\ntypedef uint64_t SnxU64;')
prelude+='\nextern V LLVMBuildAShr(LLVMBuilderRef,V,V,const char*),LLVMBuildAnd(LLVMBuilderRef,V,V,const char*),LLVMBuildOr(LLVMBuilderRef,V,V,const char*),LLVMBuildXor(LLVMBuilderRef,V,V,const char*);\n'
code=prelude + r'''
static LLVMTypeRef lane_type(SnxIr* ir,unsigned size){return LLVMIntTypeInContext(ir->ctx,8u<<size);}
static V broadcast(SnxIr* ir,V scalar,unsigned n){(void)ir;V values[16];for(unsigned i=0;i<n;++i)values[i]=scalar;return LLVMConstVector(values,n);}
static V splat_like(SnxIr*ir,V x,SnxU64 value){unsigned lanes=LLVMGetVectorSize(LLVMTypeOf(x));LLVMTypeRef type=LLVMGetElementType(LLVMTypeOf(x));return broadcast(ir,LLVMConstInt(type,value,0),lanes);}
''' + body + r'''
static void build(SnxIr*ir,unsigned lanes,unsigned uns,unsigned round){
    char name[48];sprintf(name,"h_%u_%u_%u",lanes,uns,round);
    LLVMTypeRef ptr=LLVMPointerTypeInContext(ir->ctx,0),types[3]={ptr,ptr,ptr};
    V fn=LLVMAddFunction(ir->mod,name,LLVMFunctionType(LLVMVoidTypeInContext(ir->ctx),types,3,0));
    LLVMPositionBuilderAtEnd(ir->b,LLVMAppendBasicBlockInContext(ir->ctx,fn,"entry"));
    LLVMTypeRef vec=LLVMVectorType(LLVMIntTypeInContext(ir->ctx,32),lanes);
    V a=LLVMBuildLoad2(ir->b,vec,LLVMGetParam(fn,1),"");
    V b=LLVMBuildLoad2(ir->b,vec,LLVMGetParam(fn,2),"");
    LLVMSetAlignment(a,1);LLVMSetAlignment(b,1);
    V result=halving(ir,uns,round,2,lanes,a,b);
    V store=LLVMBuildStore(ir->b,result,LLVMGetParam(fn,0));LLVMSetAlignment(store,1);
    LLVMBuildRetVoid(ir->b);
}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();
    LLVMContextRef ctx=LLVMContextCreate();LLVMModuleRef mod=LLVMModuleCreateWithNameInContext("neon_halving",ctx);
    LLVMBuilderRef builder=LLVMCreateBuilderInContext(ctx);
    SnxIr ir={.ctx=ctx,.mod=mod,.b=builder,.t_i8=LLVMInt8TypeInContext(ctx),.t_i64=LLVMInt64TypeInContext(ctx),.t_i128=LLVMInt128TypeInContext(ctx)};
    for(unsigned lanes=2;lanes<=4;lanes+=2)for(unsigned uns=0;uns<2;++uns)for(unsigned round=0;round<2;++round)build(&ir,lanes,uns,round);
    char*err=NULL;if(LLVMVerifyModule(mod,LLVMReturnStatusAction,&err)){fprintf(stderr,"halving IR invalid: %s\n",err);return 2;}
    LLVMExecutionEngineRef engine=NULL;if(LLVMCreateJITCompilerForModule(&engine,mod,2,&err)){fprintf(stderr,"LLVM JIT: %s\n",err);return 3;}
    uint32_t state=0x73af21d9;unsigned cases=0;
    for(unsigned lanes=2;lanes<=4;lanes+=2)for(unsigned uns=0;uns<2;++uns)for(unsigned round=0;round<2;++round){
        char name[48];sprintf(name,"h_%u_%u_%u",lanes,uns,round);
        void(*fn)(uint32_t*,const uint32_t*,const uint32_t*)=(void*)(uintptr_t)LLVMGetFunctionAddress(engine,name);
        if(!fn)return 4;
        for(unsigned iter=0;iter<10000;++iter){
            uint32_t x[4]={0},y[4]={0},out[4]={0},expect[4]={0};
            for(unsigned lane=0;lane<lanes;++lane){
                state=1664525u*state+1013904223u;x[lane]=state;
                state=1664525u*state+1013904223u;y[lane]=state;
                if(uns){uint64_t sum=(uint64_t)x[lane]+y[lane]+round;expect[lane]=(uint32_t)(sum>>1);}
                else {int64_t sum=(int64_t)(int32_t)x[lane]+(int32_t)y[lane]+round;expect[lane]=(uint32_t)(sum>>1);}
            }
            fn(out,x,y);
            for(unsigned lane=0;lane<lanes;++lane)if(out[lane]!=expect[lane]){
                fprintf(stderr,"halving mismatch %s iter=%u lane=%u got=%x expected=%x\n",name,iter,lane,out[lane],expect[lane]);return 5;
            }
            ++cases;
        }
    }
    printf("PASS NEON halving: %u LLVM-JIT randomized 32-bit vector cases, signed/unsigned, rounded/unrounded\n",cases);
    return 0;
}
'''
Path(sys.argv[1]).write_text(code)
