from pathlib import Path
root=Path(__file__).resolve().parent;src=(root.parents[1]/'src/common/cpu/recompiler/llvm/ir_decoder.c').read_text();sign='const SnxA64Pattern* ir_case_pattern('
st=src.index(sign);start=src.index('{',st);j=start+1;lvl=1
while lvl:
 if src[j]=='{':lvl+=1
 if src[j]=='}':lvl-=1
 j+=1
fun=src[st:j]
header='''#include "common/cpu/aarch64/a64.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef struct { uintptr_t first,last; const SnxA64Pattern* pattern; } SnxIrCaseRange;
typedef struct { size_t count; SnxIrCaseRange* ranges; } SnxIrDecoder;
typedef struct { SnxIrDecoder decoder; } SnxIr;
'''
main='''
static unsigned long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000ull+t.tv_nsec;}
int main(void){
 size_t count, checked=0;const SnxA64Pattern* patterns=snx_a64_patterns(&count);
 SnxIr ir={.decoder.count=count,.decoder.ranges=calloc(count,sizeof(*ir.decoder.ranges))};
 for(size_t i=0;i<count;++i){
   const SnxA64Pattern* p=patterns+i;
   SnxIrCaseRange range={(uintptr_t)p->cases,(uintptr_t)(p->cases+p->case_count),p};
   size_t j=i;while(j && ir.decoder.ranges[j-1].first>range.first){ir.decoder.ranges[j]=ir.decoder.ranges[j-1];--j;}ir.decoder.ranges[j]=range;
 }
 for(size_t i=0;i<count;++i){
   const SnxA64Pattern* pattern=patterns+i;
   for(size_t k=0;k<pattern->case_count;++k){
     size_t actual=(size_t)-1,expected=(size_t)-1;
     const SnxA64Case* cas=pattern->cases+k;
     const SnxA64Pattern* one=ir_case_pattern(&ir,cas,&actual);
     const SnxA64Pattern* two=snx_a64_case_pattern(cas,&expected);
     if(one!=two||actual!=expected||actual!=k){fprintf(stderr,"FAIL pattern %s case %zu: %zu != %zu\\n",pattern->name,k,actual,expected);return 1;}
     ++checked;
   }
 }
 size_t bogus; if(ir_case_pattern(&ir,(const SnxA64Case*)0,&bogus)!=NULL || bogus!=0){fprintf(stderr,"unknown case not rejected\\n");return 2;}
 unsigned seed=0xC0FFEE;
 for(size_t k=0;k<200000;++k){
   seed=seed*1664525u+1013904223u;
   const SnxA64Case* cas=snx_a64_decode(seed);
   size_t a=55,b=77;
   const SnxA64Pattern* old=snx_a64_case_pattern(cas,&a);
   const SnxA64Pattern* indexed=ir_case_pattern(&ir,cas,&b);
   if(old!=indexed||a!=b){fprintf(stderr,"FAIL decoded word %08x: %zu != %zu\\n",seed,a,b);return 3;}
 }

 const size_t rounds=1000000;
 volatile size_t checksum=0;
 unsigned long long t0=now();for(size_t k=0;k<rounds;++k){size_t idx; const SnxA64Pattern* p=snx_a64_case_pattern(patterns[k%count].cases,&idx);checksum+=p!=NULL?idx+1:0;}unsigned long long t1=now();
 for(size_t k=0;k<rounds;++k){size_t idx; const SnxA64Pattern* p=ir_case_pattern(&ir,patterns[k%count].cases,&idx);checksum+=p!=NULL?idx+1:0;}unsigned long long t2=now();
 printf("PASS %zu pattern families; %zu case descriptors plus 200000 decoded words\\n",count,checked);
 printf("Case lookup synthetic comparison (%zu operations): original %.2f ns/op, indexed %.2f ns/op; checksum %zu\\n",rounds,(t1-t0)/(double)rounds,(t2-t1)/(double)rounds,(size_t)checksum);
 free(ir.decoder.ranges);return 0;
}
'''
(root/'llvm_case_lookup_test.c').write_text(header+fun+main)
