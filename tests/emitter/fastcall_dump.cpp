#include "common/emitter/x86emitter.h"
#ifdef USE_SHIM
#include "tests/emitter/reference/x86emitter_shim.h"
#endif
#include <cstdio>
#include <sys/mman.h>
using namespace x86Emitter;
static u8* B;
static void dump(const char* nm){
    printf("%s:",nm);
    for(u8* q=B;q<x86Ptr;q++) printf(" %02x",*q);
    printf("\n");
}
int main(){
    B=(u8*)mmap((void*)0x200000000ull,1<<20,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    const void* nt=(const void*)0x200010000ull;
    const void* ft=(const void*)0x900000000ull;
    char nm[96];
    for(int t=0;t<2;t++){
      const void* f = t?ft:nt;
      for(int a=0;a<8;a++) for(int b=0;b<8;b++){
        snprintf(nm,sizeof nm,"rr%d_%d_%d",t,a,b);
        x86Ptr = (u8*)(B); xFastCall(f,xRegister32(a),xRegister32(b)); dump(nm);
        snprintf(nm,sizeof nm,"rq%d_%d_%d",t,a,b);
        x86Ptr = (u8*)(B); xFastCall(f,xRegister64(a),xRegister64(b)); dump(nm); }
      snprintf(nm,sizeof nm,"imr%d",t); x86Ptr = (u8*)(B); xFastCall(f,0x1234u,xRegister32(3)); dump(nm);
      snprintf(nm,sizeof nm,"mem%d",t); x86Ptr = (u8*)(B); xFastCall(f,ptr32[(void*)0x200020000ull]); dump(nm);
      snprintf(nm,sizeof nm,"ii%d",t);  x86Ptr = (u8*)(B); xFastCall(f,5u,9u); dump(nm);
      snprintf(nm,sizeof nm,"vp%d",t);  x86Ptr = (u8*)(B); xFastCall(f,(void*)0x200030000ull); dump(nm);
    }
    return 0; }
