from pathlib import Path
root=Path(__file__).resolve().parent
source=root.parents[1]/'src/common/cpu/recompiler/llvm'
src=(source/'ir_simd.c').read_text()+'\n'+(source/'ir_neon_memory.inc').read_text()
def function(signature):
 st=src.rindex(signature); opening=src.index('{',st);lvl=1;i=opening+1
 while lvl:
  if src[i]=='{':lvl+=1
  if src[i]=='}':lvl-=1
  i+=1
 return src[st:i]
header='''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void *LLVMContextRef,*LLVMModuleRef,*LLVMBuilderRef,*LLVMValueRef,*LLVMTypeRef,*LLVMBasicBlockRef,*LLVMExecutionEngineRef;
typedef LLVMValueRef V;
#define LLVMVectorTypeKind 13
#define LLVMReturnStatusAction 2
extern LLVMContextRef LLVMContextCreate(void);
extern LLVMModuleRef LLVMModuleCreateWithNameInContext(const char*,LLVMContextRef);
extern LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef);
extern LLVMTypeRef LLVMInt8TypeInContext(LLVMContextRef),LLVMInt64TypeInContext(LLVMContextRef),LLVMInt128TypeInContext(LLVMContextRef),LLVMVoidTypeInContext(LLVMContextRef),LLVMPointerTypeInContext(LLVMContextRef,unsigned),LLVMIntTypeInContext(LLVMContextRef,unsigned),LLVMVectorType(LLVMTypeRef,unsigned),LLVMFunctionType(LLVMTypeRef, LLVMTypeRef*, unsigned,int);
extern LLVMTypeRef LLVMTypeOf(LLVMValueRef),LLVMGetElementType(LLVMTypeRef);
extern int LLVMGetTypeKind(LLVMTypeRef);
extern unsigned LLVMGetVectorSize(LLVMTypeRef),LLVMGetIntTypeWidth(LLVMTypeRef);
extern LLVMValueRef LLVMBuildInsertElement(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,LLVMValueRef,const char*),LLVMBuildShuffleVector(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,LLVMValueRef,const char*),LLVMBuildBitCast(LLVMBuilderRef,LLVMValueRef,LLVMTypeRef,const char*),LLVMBuildZExt(LLVMBuilderRef,LLVMValueRef,LLVMTypeRef,const char*),LLVMBuildTrunc(LLVMBuilderRef,LLVMValueRef,LLVMTypeRef,const char*),LLVMBuildLShr(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,const char*),LLVMBuildStore(LLVMBuilderRef,LLVMValueRef,LLVMValueRef),LLVMBuildInBoundsGEP2(LLVMBuilderRef,LLVMTypeRef,LLVMValueRef,LLVMValueRef*,unsigned,const char*),LLVMBuildRetVoid(LLVMBuilderRef),LLVMGetParam(LLVMValueRef,unsigned),LLVMAddFunction(LLVMModuleRef,const char*,LLVMTypeRef),LLVMGetUndef(LLVMTypeRef),LLVMConstInt(LLVMTypeRef,unsigned long long,int),LLVMConstVector(LLVMValueRef*,unsigned),LLVMBuildLoad2(LLVMBuilderRef,LLVMTypeRef,LLVMValueRef,const char*);
extern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(LLVMContextRef,LLVMValueRef,const char*);
extern void LLVMPositionBuilderAtEnd(LLVMBuilderRef,LLVMBasicBlockRef),LLVMSetAlignment(LLVMValueRef,unsigned);
extern int LLVMIsConstant(LLVMValueRef),LLVMVerifyModule(LLVMModuleRef,int,char**);
extern char *LLVMPrintModuleToString(LLVMModuleRef);
extern void LLVMDisposeMessage(char*);
extern void LLVMInitializeX86TargetInfo(void),LLVMInitializeX86Target(void),LLVMInitializeX86TargetMC(void),LLVMInitializeX86AsmPrinter(void);
extern int LLVMCreateJITCompilerForModule(LLVMExecutionEngineRef*,LLVMModuleRef,unsigned,char**);
extern unsigned long long LLVMGetFunctionAddress(LLVMExecutionEngineRef,const char*);
typedef struct { LLVMContextRef ctx; LLVMModuleRef mod; LLVMBuilderRef b; LLVMTypeRef t_i64,t_i128,t_i8; LLVMValueRef c; } SnxIr;
#define B ir->b
static LLVMValueRef ir_i32(SnxIr*ir,unsigned v){return LLVMConstInt(LLVMIntTypeInContext(ir->ctx,32),v,0);}
static LLVMValueRef ir_i64(SnxIr*ir,unsigned long long v){return LLVMConstInt(ir->t_i64,v,0);}
static void ir_set_vd(SnxIr*ir,unsigned rd,unsigned lane,LLVMValueRef value){LLVMValueRef off=ir_i64(ir,rd*16+lane*8);LLVMValueRef dst=LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&off,1,"");LLVMValueRef st=LLVMBuildStore(ir->b,value,dst);LLVMSetAlignment(st,8);}
'''
body='\n\n'.join([function('static V broadcast('),function('static void write_lanes(')])
main='''
static void (*generate(LLVMModuleRef m,LLVMContextRef ctx,LLVMBuilderRef b,unsigned width,unsigned n,unsigned q))(uint64_t*,uint64_t) {
    static SnxIr ir;
    ir=(SnxIr){.ctx=ctx,.mod=m,.b=b,.t_i64=LLVMInt64TypeInContext(ctx),.t_i128=LLVMInt128TypeInContext(ctx),.t_i8=LLVMInt8TypeInContext(ctx)};
    LLVMTypeRef ty[2]={LLVMPointerTypeInContext(ctx,0),ir.t_i64};
    char name[40];sprintf(name,"write_%u_%u_%u",width,n,q);
    LLVMValueRef fn=LLVMAddFunction(m,name,LLVMFunctionType(LLVMVoidTypeInContext(ctx),ty,2,0));
    LLVMPositionBuilderAtEnd(b,LLVMAppendBasicBlockInContext(ctx,fn,"entry"));
    ir.c=LLVMGetParam(fn,0);
    V arg=LLVMGetParam(fn,1);
    if(width<64) arg=LLVMBuildTrunc(b,arg,LLVMIntTypeInContext(ctx,width),"");
    V v=broadcast(&ir,arg,n);
    write_lanes(&ir,0,v,q);
    LLVMBuildRetVoid(b);
    return 0;
}
int main(void){
    LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();
    LLVMContextRef ctx=LLVMContextCreate();LLVMModuleRef m=LLVMModuleCreateWithNameInContext("neon",ctx);LLVMBuilderRef b=LLVMCreateBuilderInContext(ctx);
    const unsigned widths[]={8,16,32,64,8,16,32,64};
    const unsigned counts[]={8,4,2,1,16,8,4,2};
    const unsigned qs[]={0,0,0,0,1,1,1,1};
    for(int i=0;i<8;++i)generate(m,ctx,b,widths[i],counts[i],qs[i]);
    char *message=NULL;
    if(LLVMVerifyModule(m,LLVMReturnStatusAction,&message)){fprintf(stderr,"INVALID IR: %s\\n",message?message:"");return 1;}
    LLVMExecutionEngineRef engine=NULL;
    if(LLVMCreateJITCompilerForModule(&engine,m,2,&message)){fprintf(stderr,"JIT failed: %s\\n",message);return 2;}
    for(int i=0;i<8;++i){
        char name[40];sprintf(name,"write_%u_%u_%u",widths[i],counts[i],qs[i]);
        void (*fn)(uint64_t*,uint64_t)=(void (*)(uint64_t*,uint64_t))(uintptr_t)LLVMGetFunctionAddress(engine,name);
        if(!fn){fprintf(stderr,"missing fn: %s\\n",name);return 3;}
        uint64_t words[2]={~0ull,~0ull};uint64_t v=0xEFCDAB8967452301ull;
        fn(words,v);
        unsigned used=widths[i]*counts[i];
        uint8_t *p=(uint8_t*)words;
        for(unsigned j=0;j<16;++j){
            unsigned bit=8*j;
            uint8_t expected=(j*8<used && (qs[i] || j<8))? (uint8_t)(v>>(8*(j%(widths[i]/8)))):0;
            if(p[j]!=expected){fprintf(stderr,"WRONG %s byte %u = %02x expected %02x\\n",name,j,p[j],expected);return 4;}
        }
        printf("PASS %-16s %016llx %016llx\\n",name,(unsigned long long)words[0],(unsigned long long)words[1]);
    }
    return 0;
}
'''
(root/'llvm_neon_real.c').write_text(header+body+main)
