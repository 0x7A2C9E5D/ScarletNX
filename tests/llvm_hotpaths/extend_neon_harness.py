from pathlib import Path
p=Path(__file__).resolve().parent/'llvm_neon_real.c';s=p.read_text();s=s.replace('extern LLVMValueRef LLVMBuildInsertElement(','''extern LLVMValueRef LLVMBuildAdd(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,const char*),LLVMBuildSub(LLVMBuilderRef,LLVMValueRef,LLVMValueRef,const char*);
extern LLVMValueRef LLVMBuildInsertElement(''')
start=s.index('int main(void){')
s=s[:start]+'''static void vector_op(LLVMModuleRef m,LLVMContextRef ctx,LLVMBuilderRef b,unsigned bits,unsigned n,unsigned q,unsigned sub){
    SnxIr ir={.ctx=ctx,.mod=m,.b=b,.t_i8=LLVMInt8TypeInContext(ctx),.t_i64=LLVMInt64TypeInContext(ctx),.t_i128=LLVMInt128TypeInContext(ctx)};
    LLVMTypeRef p=LLVMPointerTypeInContext(ctx,0), args[3]={p,p,p};
    char name[40];sprintf(name,"vector_%u_%u_%u_%u",bits,n,q,sub);
    V fn=LLVMAddFunction(m,name,LLVMFunctionType(LLVMVoidTypeInContext(ctx),args,3,0));
    LLVMPositionBuilderAtEnd(b,LLVMAppendBasicBlockInContext(ctx,fn,"entry"));
    ir.c=LLVMGetParam(fn,0);
    LLVMTypeRef vec=LLVMVectorType(LLVMIntTypeInContext(ctx,bits),n);
    V a=LLVMBuildLoad2(b,vec,LLVMGetParam(fn,1),"");V d=LLVMBuildLoad2(b,vec,LLVMGetParam(fn,2),"");
    LLVMSetAlignment(a,1);LLVMSetAlignment(d,1);
    write_lanes(&ir,0,sub?LLVMBuildSub(b,a,d,""):LLVMBuildAdd(b,a,d,""),q);
    LLVMBuildRetVoid(b);
}
''' +s[start:]
s=s.replace('    for(int i=0;i<8;++i)generate(m,ctx,b,widths[i],counts[i],qs[i]);','''    for(int i=0;i<8;++i)generate(m,ctx,b,widths[i],counts[i],qs[i]);
    for(unsigned size=0;size<=3;++size)for(unsigned q=0;q<=1;++q){
       if(size==3 && !q)continue;
       for(unsigned sub=0;sub<=1;++sub)vector_op(m,ctx,b,8u<<size,(q?16u:8u)>>size,q,sub);
    }''')
s=s.replace('    return 0;\n}\n', '''    return 0;
}
''')
start=s.rindex('    return 0;\n}')
s=s[:start]+'''    uint32_t seed=0xDEADBEEF;
    for(unsigned size=0;size<=3;++size)for(unsigned q=0;q<=1;++q){
      if(size==3&&!q)continue;
      unsigned bits=8u<<size, lanes=(q?16u:8u)>>size;
      for(unsigned sub=0;sub<=1;++sub){
        char name[40];sprintf(name,"vector_%u_%u_%u_%u",bits,lanes,q,sub);
        void(*fn)(void*,void*,void*)=(void(*)(void*,void*,void*))(uintptr_t)LLVMGetFunctionAddress(engine,name);
        if(!fn){fprintf(stderr,"missing %s\\n",name);return 10;}
        for(unsigned k=0;k<1000;++k){
          uint8_t a[16],b[16],out[16],expected[16]={0};
          for(unsigned j=0;j<16;++j){seed=1664525*seed+1013904223;a[j]=seed>>24;seed=1664525*seed+1013904223;b[j]=seed>>24;}
          for(unsigned j=0;j<lanes;++j){
            uint64_t x=0,y=0;
            memcpy(&x,a+j*(bits/8),bits/8);memcpy(&y,b+j*(bits/8),bits/8);
            uint64_t value=sub?x-y:x+y;
            memcpy(expected+j*(bits/8),&value,bits/8);
          }
          memset(out,0xA5,sizeof(out));fn(out,a,b);
          if(memcmp(out,expected,16)){fprintf(stderr,"mismatch %s iteration %u\\n",name,k);return 11;}
        }
        printf("PASS %-16s 1000 randomized iterations\\n",name);
      }
    }
''' +s[start:]
p.write_text(s)
