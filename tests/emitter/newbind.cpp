#include "common/emitter/x86emitter.h"
#include "common/emitter/x86emitter_shim.h"
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
using namespace x86Emitter;
static u8* B; static long C=0,F=0; static int NF=0;
template <typename A,typename Bf> static void ck(const char* w,A a,Bf b){
    u8 x[40],y[40]; size_t nx,ny;
    xSetPtr(B); a(); nx=xGetPtr()-B; if(nx>40)nx=40; memcpy(x,B,nx);
    memset(B,0xcc,40); xSetPtr(B); b(); ny=xGetPtr()-B; if(ny>40)ny=40; memcpy(y,B,ny);
    C++;
    if(nx!=ny||memcmp(x,y,nx)){ F++; if(NF<10){ printf("  %-26s ref[%zu]:",w,nx);
      for(size_t i=0;i<nx;i++)printf(" %02x",x[i]); printf("  c89[%zu]:",ny);
      for(size_t i=0;i<ny;i++)printf(" %02x",y[i]); printf("\n"); NF++; } } }
int main(){
    B=(u8*)mmap((void*)0x200000000ull,1<<20,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    char nm[128];
    /* xPSRA: the shift family without Q -- newly bound, never verified */
    { const shim_ShiftNoQ s={{0x66,0xe1,0x71,4},{0x66,0xe2,0x72,4}};
      const u8 im[6]={0,1,2,7,0x40,0xff};
      for(int a=0;a<16;a++){
        for(int b=0;b<16;b++){
          snprintf(nm,sizeof nm,"PSRA.W x%d,x%d",a,b);
          ck(nm,[&]{ xPSRA.W(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ s.W(xRegisterSSE(a),xRegisterSSE(b)); });
          snprintf(nm,sizeof nm,"PSRA.D x%d,x%d",a,b);
          ck(nm,[&]{ xPSRA.D(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ s.D(xRegisterSSE(a),xRegisterSSE(b)); }); }
        for(int i=0;i<6;i++){
          snprintf(nm,sizeof nm,"PSRA.W imm x%d i%d",a,i);
          ck(nm,[&]{ xPSRA.W(xRegisterSSE(a),im[i]); },[&]{ s.W(xRegisterSSE(a),im[i]); });
          snprintf(nm,sizeof nm,"PSRA.D imm x%d i%d",a,i);
          ck(nm,[&]{ xPSRA.D(xRegisterSSE(a),im[i]); },[&]{ s.D(xRegisterSSE(a),im[i]); }); } } }
    /* indirect jmp/call through memory */
    { const shim_JmpCall sJ={true}, sC={false};
      for(int b=0;b<16;b++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptrNative[xAddressVoid(xAddressReg(b),xAddressReg(ix),4,0x18)]; };
        snprintf(nm,sizeof nm,"JMP_ind b%d i%d",b,ix);
        ck(nm,[&]{ xJMP(M()); },[&]{ sJ(M()); });
        snprintf(nm,sizeof nm,"CALL_ind b%d i%d",b,ix);
        ck(nm,[&]{ xCALL(M()); },[&]{ sC(M()); }); }
      for(int r=0;r<16;r++){
        snprintf(nm,sizeof nm,"JMP_r %d",r);
        ck(nm,[&]{ xJMP(xAddressReg(r)); },[&]{ sJ(xAddressReg(r)); });
        snprintf(nm,sizeof nm,"CALL_r %d",r);
        ck(nm,[&]{ xCALL(xAddressReg(r)); },[&]{ sC(xAddressReg(r)); }); } }
    /* movsx / movzx: all six overloads the reference declares -- three
       source widths against register and memory, with the 16-bit
       destination's 0x66 prefix and movsxd's plain 0x63 opcode. Binding
       only two of the six compiled here and broke eleven other TUs. */
    { const shim_MovExtend sS6={true}, sZ6={false};
      static const int q8[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x14,0x15,0x16,0x17};
      for(int d=0;d<16;d++){
        for(int k=0;k<20;k++){
          snprintf(nm,sizeof nm,"SX16_8 %d %x",d,q8[k]);
          ck(nm,[&]{xMOVSX(xRegister16(d),xRegister8(q8[k]));},[&]{sS6(xRegister16(d),xRegister8(q8[k]));});
          snprintf(nm,sizeof nm,"SX64_8 %d %x",d,q8[k]);
          ck(nm,[&]{xMOVSX(xRegister64(d),xRegister8(q8[k]));},[&]{sS6(xRegister64(d),xRegister8(q8[k]));}); }
        for(int k=0;k<16;k++){
          snprintf(nm,sizeof nm,"ZX64_16 %d %d",d,k);
          ck(nm,[&]{xMOVZX(xRegister64(d),xRegister16(k));},[&]{sZ6(xRegister64(d),xRegister16(k));});
          snprintf(nm,sizeof nm,"SXD64_32 %d %d",d,k);
          ck(nm,[&]{xMOVSX(xRegister64(d),xRegister32(k));},[&]{sS6(xRegister64(d),xRegister32(k));}); }
        for(int b=0;b<16;b++){
          auto M8 =[&]{return ptr8 [xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
          auto M16=[&]{return ptr16[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
          auto M32=[&]{return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
          snprintf(nm,sizeof nm,"SX32_m8 %d b%d",d,b);
          ck(nm,[&]{xMOVSX(xRegister32(d),M8());},[&]{sS6(xRegister32(d),M8());});
          snprintf(nm,sizeof nm,"ZX16_m8 %d b%d",d,b);
          ck(nm,[&]{xMOVZX(xRegister16(d),M8());},[&]{sZ6(xRegister16(d),M8());});
          snprintf(nm,sizeof nm,"SX64_m16 %d b%d",d,b);
          ck(nm,[&]{xMOVSX(xRegister64(d),M16());},[&]{sS6(xRegister64(d),M16());});
          snprintf(nm,sizeof nm,"SXD64_m32 %d b%d",d,b);
          ck(nm,[&]{xMOVSX(xRegister64(d),M32());},[&]{sS6(xRegister64(d),M32());}); } } }

    /* movsx / movzx: every source width and register */
    { const shim_MovExtend sS={true}, sZ={false};
      static const int i8[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x14,0x15,0x16,0x17};
      for(int d=0;d<16;d++){
        for(int k=0;k<20;k++){
          snprintf(nm,sizeof nm,"MOVSX32_8 %d %x",d,i8[k]);
          ck(nm,[&]{ xMOVSX(xRegister32(d),xRegister8(i8[k])); },[&]{ sS(xRegister32(d),xRegister8(i8[k])); });
          snprintf(nm,sizeof nm,"MOVZX32_8 %d %x",d,i8[k]);
          ck(nm,[&]{ xMOVZX(xRegister32(d),xRegister8(i8[k])); },[&]{ sZ(xRegister32(d),xRegister8(i8[k])); }); }
        for(int k=0;k<16;k++){
          snprintf(nm,sizeof nm,"MOVSX32_16 %d %d",d,k);
          ck(nm,[&]{ xMOVSX(xRegister32(d),xRegister16(k)); },[&]{ sS(xRegister32(d),xRegister16(k)); });
          snprintf(nm,sizeof nm,"MOVZX32_16 %d %d",d,k);
          ck(nm,[&]{ xMOVZX(xRegister32(d),xRegister16(k)); },[&]{ sZ(xRegister32(d),xRegister16(k)); }); } } }
    printf("newly bound: cases %ld | divergent %ld\n",C,F);
    return F?1:0; }
