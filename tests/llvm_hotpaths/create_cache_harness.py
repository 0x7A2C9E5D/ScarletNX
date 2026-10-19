from pathlib import Path
root=Path(__file__).resolve().parent;src=(root.parents[1]/'src/common/cpu/recompiler/llvm/ir_state.c').read_text()
def function(signature):
 st=src.index(signature);open_=src.index('{',st);j=open_+1;level=1
 while level:
  if src[j]=='{': level+=1
  if src[j]=='}': level-=1
  j+=1
 return src[st:j]
# reuse exact LLVM API prototypes and test IR helpers in neon harness
prelude=(root/'llvm_neon_real.c').read_text().split('static V broadcast(')[0]
prelude="#include <stddef.h>\n"+prelude
prelude=prelude.replace('typedef struct { LLVMContextRef ctx; LLVMModuleRef mod; LLVMBuilderRef b; LLVMTypeRef t_i64,t_i128,t_i8; LLVMValueRef c; } SnxIr;', '''typedef struct { uint64_t x[32]; uint64_t flags; } CPU;
typedef struct { LLVMBasicBlockRef block; LLVMValueRef x[32]; uint32_t valid; } SnxIrRegisterCache;
typedef struct { SnxIrRegisterCache registers; } SnxIrBlockState;
typedef struct { LLVMContextRef ctx; LLVMModuleRef mod; LLVMBuilderRef b; LLVMTypeRef t_i64,t_i128,t_i8; LLVMValueRef c; SnxIrBlockState state; } SnxIr;''')
prelude=prelude.replace('extern LLVMValueRef LLVMBuildInsertElement(', 'extern LLVMValueRef LLVMBuildAdd(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,const char*);\nextern LLVMValueRef LLVMBuildInsertElement(')
prelude=prelude.replace('extern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(', 'extern LLVMBasicBlockRef LLVMGetInsertBlock(LLVMBuilderRef);\nextern LLVMBasicBlockRef LLVMAppendBasicBlockInContext(')
prelude=prelude.replace('static void ir_set_vd(', 'static void ir_set_vd(')
# define minimal CPU interface functions used by extracted cache
func='''
static void *ir_cpu(SnxIr*ir,size_t off){LLVMValueRef ptr=ir_i64(ir,off);return LLVMBuildInBoundsGEP2(ir->b,ir->t_i8,ir->c,&ptr,1,"");}
static size_t ir_off_x(unsigned n){return offsetof(CPU,x)+8u*n;}
static size_t ir_off_fpcr(void){return offsetof(CPU,flags)+16;}
static void ir_fp_guards_invalidate(SnxIr*ir){(void)ir;}
static const size_t flag_offsets[4]={0,4,8,12};
static void ir_flags_invalidate(SnxIr*ir){(void)ir;}
static LLVMValueRef ir_u64(SnxIr*ir,LLVMValueRef v){(void)ir;return v;}
static LLVMValueRef ir_tag_cpu(SnxIr*ir,LLVMValueRef v){(void)ir;return v;}
static LLVMValueRef ir_cpu_load(SnxIr*ir,LLVMTypeRef t,size_t off){return LLVMBuildLoad2(ir->b,t,ir_cpu(ir,off),"");}
'''
# LLVMBuildStore return LLVMValueRef; LLVMBuildLoad2 correct.
body='\n\n'.join([function('void ir_registers_invalidate('),function('static void registers_block('),function('void ir_cpu_store('),function('LLVMValueRef ir_x('),function('void ir_set_x(')])
# Need LLVMConstInt and LLVMBuildTrunc already declared.
main='''
int main(void){
 LLVMInitializeX86TargetInfo();LLVMInitializeX86Target();LLVMInitializeX86TargetMC();LLVMInitializeX86AsmPrinter();
 LLVMContextRef ctx=LLVMContextCreate();LLVMModuleRef m=LLVMModuleCreateWithNameInContext("regs",ctx);LLVMBuilderRef b=LLVMCreateBuilderInContext(ctx);
 SnxIr ir={.ctx=ctx,.mod=m,.b=b,.t_i64=LLVMInt64TypeInContext(ctx),.t_i128=LLVMInt128TypeInContext(ctx),.t_i8=LLVMInt8TypeInContext(ctx)};
 LLVMTypeRef p=LLVMPointerTypeInContext(ctx,0);
 LLVMValueRef fn=LLVMAddFunction(m,"regs",LLVMFunctionType(LLVMVoidTypeInContext(ctx),&p,1,0));
 LLVMPositionBuilderAtEnd(b,LLVMAppendBasicBlockInContext(ctx,fn,"start"));ir.c=LLVMGetParam(fn,0);
 LLVMValueRef a=ir_x(&ir,0);if(ir_x(&ir,0)!=a){puts("duplicate load in same block");return 1;}
 ir_set_x(&ir,1,a);if(ir_x(&ir,0)!=a){puts("cache lost on disjoint GPR store");return 2;}
 ir_cpu_store(&ir,ir_i64(&ir,33),offsetof(CPU,flags));if(ir_x(&ir,0)!=a){puts("cache lost on flags store");return 3;}
 LLVMValueRef added=LLVMBuildAdd(b,a,ir_i64(&ir,1),"");ir_set_x(&ir,0,added);
 if(ir_x(&ir,0)!=added){puts("read failed to forward written SSA value");return 4;}
 ir_cpu_store(&ir,ir_i64(&ir,13),ir_off_x(0));if(ir_x(&ir,0)==added){puts("direct CPU state write did not invalidate");return 5;}
 LLVMValueRef sp=ir_i64(&ir,77);ir_set_x(&ir,31,sp);
 if(ir_x(&ir,31)!=sp){puts("high register validity bit was lost");return 9;}
 ir_registers_invalidate(&ir);
 if(ir_x(&ir,31)==sp){puts("cache survived explicit invalidation");return 10;}
 LLVMBuildRetVoid(b);
 LLVMPositionBuilderAtEnd(b,LLVMAppendBasicBlockInContext(ctx,fn,"second"));
 if(ir_x(&ir,0)==added){puts("cache crossed a basic block");return 6;}
 LLVMBuildRetVoid(b);
 char *error=NULL;
 if(LLVMVerifyModule(m,LLVMReturnStatusAction,&error)){fprintf(stderr,"IR invalid %s\\n",error);return 7;}
 char *str=LLVMPrintModuleToString(m);
 int loads=0;for(char*line=str;*line;line++)if(!strncmp(line,"load i64,",9))++loads;
 if(loads<2){puts("expected CPU loads were not emitted");return 8;}
 printf("PASS GPR SSA cache: forwarding, independent writes, direct writes, block boundaries; IR bytes=%zu\\n",strlen(str));
 return 0;
}
'''
# Invalid second basic block reachable? unattached block from fn with ret is valid unreachable block, yes.
(root/'llvm_gpr_cache_test.c').write_text(prelude+func+body+main)
