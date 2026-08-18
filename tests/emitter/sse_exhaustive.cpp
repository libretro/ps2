/* SSE comparison, generated from the reference's own tables. Member names and
 * {prefix, opcode} pairs come out of simd.cpp; whether a member takes an imm8
 * is read from its declared type in implement/, scoped to its own struct --
 * a bare member name is ambiguous across families. Nothing is retyped, so
 * nothing can be omitted for having been forgotten. */
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
    if(nx!=ny||memcmp(x,y,nx)){ F++;
      if(NF<12){ printf("  %-28s ref[%zu]:",w,nx);
        for(size_t i=0;i<nx;i++)printf(" %02x",x[i]);
        printf("  c89[%zu]:",ny);
        for(size_t i=0;i<ny;i++)printf(" %02x",y[i]); printf("\n"); NF++; } } }
static const int SC[4]={1,2,4,8};
static const s32 DSP[5]={0,1,0x7f,0x80,-0x1000};
static const u8  IM8[5]={0x00,0x1b,0x55,0xaa,0xff};
int main(){
    B=(u8*)mmap((void*)0x200000000ull,1<<20,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    void* const ABS[3]={(void*)0x200001000ull,(void*)0x410000ull,(void*)0x1fff0000ull};
    char nm[160];
    { const shim_SimdRegSSE sh={0x66,0xd4};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PADD.Q x%d,x%d",a,b);
        ck(nm,[&]{ xPADD.Q(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PADD.Q_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPADD.Q(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PADD.Q_abs%d",k);
        ck(nm,[&]{ xPADD.Q(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xdc};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PADD.USB x%d,x%d",a,b);
        ck(nm,[&]{ xPADD.USB(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PADD.USB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPADD.USB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PADD.USB_abs%d",k);
        ck(nm,[&]{ xPADD.USB(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x74};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQB x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQB_abs%d",k);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x75};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQW x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQW_abs%d",k);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x76};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQD x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQD_abs%d",k);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x64};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTB x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTB_abs%d",k);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x65};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTW x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTW_abs%d",k);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x66};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTD x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTD_abs%d",k);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xd5};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.LW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.LW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.LW_abs%d",k);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xe5};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HW_abs%d",k);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xe4};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HUW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HUW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HUW_abs%d",k);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xf4};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.UDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.UDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.UDQ_abs%d",k);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x0b38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HRSW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HRSW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HRSW_abs%d",k);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x4038};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.LD x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.LD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.LD_abs%d",k);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x2838};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.DQ x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.DQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.DQ_abs%d",k);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.D x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.D(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.D_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.D(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.D_abs%d i%d",k,i);
        ck(nm,[&]{ xPSHUF.D(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0xf2,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.LW x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.LW(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.LW_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.LW(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.LW_abs%d i%d",k,i);
        ck(nm,[&]{ xPSHUF.LW(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0xf3,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.HW x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.HW(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.HW_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.HW(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.HW_abs%d i%d",k,i);
        ck(nm,[&]{ xPSHUF.HW(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]],IM8[i]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x0038};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PSHUF.B x%d,x%d",a,b);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.B_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PSHUF.B_abs%d",k);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xfb};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PSUB.Q x%d,x%d",a,b);
        ck(nm,[&]{ xPSUB.Q(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PSUB.Q_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPSUB.Q(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PSUB.Q_abs%d",k);
        ck(nm,[&]{ xPSUB.Q(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xd8};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PSUB.USB x%d,x%d",a,b);
        ck(nm,[&]{ xPSUB.USB(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PSUB.USB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPSUB.USB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PSUB.USB_abs%d",k);
        ck(nm,[&]{ xPSUB.USB(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x60};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LBW x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LBW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LBW_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x61};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LWD x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LWD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LWD_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x62};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6c};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x68};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HBW x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HBW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HBW_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x69};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HWD x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HWD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HWD_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x2e};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UCOMI.SS x%d,x%d",a,b);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UCOMI.SS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UCOMI.SS_abs%d",k);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x2e};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UCOMI.SD x%d,x%d",a,b);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(a),xRegisterSSE(b)); },
             [&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UCOMI.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UCOMI.SD_abs%d",k);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(5),ptr128[ABS[k]]); },
             [&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    printf("sse: cases %ld | divergent %ld  (31 members, 3 with imm8)\n",C,F);
    return F?1:0; }
