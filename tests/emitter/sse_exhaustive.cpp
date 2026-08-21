/* SSE comparison, generated from the reference's tables.
 * Member NAMES come from the struct's declaration order, not from the trailing
 * comments on the initialiser -- xSQRT's third entry is commented "SS" when the
 * struct declares PS, SS, SD, and trusting the comment generated a call to the
 * wrong member. Order is authoritative; comments are documentation. */
#include "common/emitter/x86emitter.h"
#include "tests/emitter/reference/x86emitter_shim.h"
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
using namespace x86Emitter;
static u8* B; static long C=0,F=0; static int NF=0;
template <typename A,typename Bf> static void ck(const char* w,A a,Bf b){
    u8 x[40],y[40]; size_t nx,ny;
    x86Ptr = (u8*)(B); a(); nx=x86Ptr-B; if(nx>40)nx=40; memcpy(x,B,nx);
    memset(B,0xcc,40); x86Ptr = (u8*)(B); b(); ny=x86Ptr-B; if(ny>40)ny=40; memcpy(y,B,ny);
    C++;
    if(nx!=ny||memcmp(x,y,nx)){ F++;
      if(NF<12){ printf("  %-26s ref[%zu]:",w,nx);
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
    { const shim_SimdRegImmSSE sh={0x66,0x0c3a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"BLEND.PS x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xBLEND.PS(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"BLEND.PS_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xBLEND.PS(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x0d3a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"BLEND.PD x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xBLEND.PD(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"BLEND.PD_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xBLEND.PD(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1438};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"BLEND.VPS x%d,x%d",a,b);
        ck(nm,[&]{ xBLEND.VPS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"BLEND.VPS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xBLEND.VPS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"BLEND.VPS_abs%d",k);
        ck(nm,[&]{ xBLEND.VPS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1538};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"BLEND.VPD x%d,x%d",a,b);
        ck(nm,[&]{ xBLEND.VPD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"BLEND.VPD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xBLEND.VPD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"BLEND.VPD_abs%d",k);
        ck(nm,[&]{ xBLEND.VPD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x403a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"DP.PS x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xDP.PS(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"DP.PS_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xDP.PS(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x413a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"DP.PD x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xDP.PD(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"DP.PD_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xDP.PD(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x5f};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MAX.PS x%d,x%d",a,b);
        ck(nm,[&]{ xMAX.PS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MAX.PS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMAX.PS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MAX.PS_abs%d",k);
        ck(nm,[&]{ xMAX.PS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x5f};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MAX.PD x%d,x%d",a,b);
        ck(nm,[&]{ xMAX.PD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MAX.PD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMAX.PD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MAX.PD_abs%d",k);
        ck(nm,[&]{ xMAX.PD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf3,0x5f};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MAX.SS x%d,x%d",a,b);
        ck(nm,[&]{ xMAX.SS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MAX.SS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMAX.SS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MAX.SS_abs%d",k);
        ck(nm,[&]{ xMAX.SS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf2,0x5f};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MAX.SD x%d,x%d",a,b);
        ck(nm,[&]{ xMAX.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MAX.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMAX.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MAX.SD_abs%d",k);
        ck(nm,[&]{ xMAX.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x5d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MIN.PS x%d,x%d",a,b);
        ck(nm,[&]{ xMIN.PS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MIN.PS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMIN.PS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MIN.PS_abs%d",k);
        ck(nm,[&]{ xMIN.PS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x5d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MIN.PD x%d,x%d",a,b);
        ck(nm,[&]{ xMIN.PD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MIN.PD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMIN.PD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MIN.PD_abs%d",k);
        ck(nm,[&]{ xMIN.PD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf3,0x5d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MIN.SS x%d,x%d",a,b);
        ck(nm,[&]{ xMIN.SS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MIN.SS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMIN.SS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MIN.SS_abs%d",k);
        ck(nm,[&]{ xMIN.SS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf2,0x5d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"MIN.SD x%d,x%d",a,b);
        ck(nm,[&]{ xMIN.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"MIN.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMIN.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"MIN.SD_abs%d",k);
        ck(nm,[&]{ xMIN.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1c38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PABS.B x%d,x%d",a,b);
        ck(nm,[&]{ xPABS.B(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PABS.B_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPABS.B(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PABS.B_abs%d",k);
        ck(nm,[&]{ xPABS.B(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1d38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PABS.W x%d,x%d",a,b);
        ck(nm,[&]{ xPABS.W(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PABS.W_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPABS.W(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PABS.W_abs%d",k);
        ck(nm,[&]{ xPABS.W(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1e38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PABS.D x%d,x%d",a,b);
        ck(nm,[&]{ xPABS.D(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PABS.D_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPABS.D(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PABS.D_abs%d",k);
        ck(nm,[&]{ xPABS.D(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x63};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PACK.SSWB x%d,x%d",a,b);
        ck(nm,[&]{ xPACK.SSWB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PACK.SSWB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPACK.SSWB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PACK.SSWB_abs%d",k);
        ck(nm,[&]{ xPACK.SSWB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6b};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PACK.SSDW x%d,x%d",a,b);
        ck(nm,[&]{ xPACK.SSDW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PACK.SSDW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPACK.SSDW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PACK.SSDW_abs%d",k);
        ck(nm,[&]{ xPACK.SSDW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x67};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PACK.USWB x%d,x%d",a,b);
        ck(nm,[&]{ xPACK.USWB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PACK.USWB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPACK.USWB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PACK.USWB_abs%d",k);
        ck(nm,[&]{ xPACK.USWB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x2b38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PACK.USDW x%d,x%d",a,b);
        ck(nm,[&]{ xPACK.USDW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PACK.USDW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPACK.USDW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PACK.USDW_abs%d",k);
        ck(nm,[&]{ xPACK.USDW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xdb};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PAND x%d,x%d",a,b);
        ck(nm,[&]{ xPAND(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PAND_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPAND(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PAND_abs%d",k);
        ck(nm,[&]{ xPAND(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xdf};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PANDN x%d,x%d",a,b);
        ck(nm,[&]{ xPANDN(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PANDN_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPANDN(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PANDN_abs%d",k);
        ck(nm,[&]{ xPANDN(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x0e3a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PBLEND.W x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPBLEND.W(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PBLEND.W_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPBLEND.W(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1038};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PBLEND.VB x%d,x%d",a,b);
        ck(nm,[&]{ xPBLEND.VB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PBLEND.VB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPBLEND.VB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PBLEND.VB_abs%d",k);
        ck(nm,[&]{ xPBLEND.VB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x74};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQB x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQB_abs%d",k);
        ck(nm,[&]{ xPCMP.EQB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x75};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQW x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQW_abs%d",k);
        ck(nm,[&]{ xPCMP.EQW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x76};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.EQD x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.EQD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.EQD_abs%d",k);
        ck(nm,[&]{ xPCMP.EQD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x64};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTB x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTB_abs%d",k);
        ck(nm,[&]{ xPCMP.GTB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x65};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTW x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTW_abs%d",k);
        ck(nm,[&]{ xPCMP.GTW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x66};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PCMP.GTD x%d,x%d",a,b);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PCMP.GTD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PCMP.GTD_abs%d",k);
        ck(nm,[&]{ xPCMP.GTD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xf5};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMADD.WD x%d,x%d",a,b);
        ck(nm,[&]{ xPMADD.WD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMADD.WD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMADD.WD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMADD.WD_abs%d",k);
        ck(nm,[&]{ xPMADD.WD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xf438};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMADD.UBSW x%d,x%d",a,b);
        ck(nm,[&]{ xPMADD.UBSW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMADD.UBSW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMADD.UBSW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMADD.UBSW_abs%d",k);
        ck(nm,[&]{ xPMADD.UBSW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xde};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.UB x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.UB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.UB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.UB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.UB_abs%d",k);
        ck(nm,[&]{ xPMAX.UB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xee};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.SW x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.SW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.SW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.SW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.SW_abs%d",k);
        ck(nm,[&]{ xPMAX.SW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3c38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.SB x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.SB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.SB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.SB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.SB_abs%d",k);
        ck(nm,[&]{ xPMAX.SB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3d38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.SD x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.SD_abs%d",k);
        ck(nm,[&]{ xPMAX.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3e38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.UW x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.UW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.UW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.UW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.UW_abs%d",k);
        ck(nm,[&]{ xPMAX.UW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3f38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMAX.UD x%d,x%d",a,b);
        ck(nm,[&]{ xPMAX.UD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMAX.UD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMAX.UD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMAX.UD_abs%d",k);
        ck(nm,[&]{ xPMAX.UD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xda};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.UB x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.UB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.UB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.UB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.UB_abs%d",k);
        ck(nm,[&]{ xPMIN.UB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xea};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.SW x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.SW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.SW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.SW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.SW_abs%d",k);
        ck(nm,[&]{ xPMIN.SW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3838};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.SB x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.SB(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.SB_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.SB(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.SB_abs%d",k);
        ck(nm,[&]{ xPMIN.SB(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3938};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.SD x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.SD_abs%d",k);
        ck(nm,[&]{ xPMIN.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3a38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.UW x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.UW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.UW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.UW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.UW_abs%d",k);
        ck(nm,[&]{ xPMIN.UW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x3b38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMIN.UD x%d,x%d",a,b);
        ck(nm,[&]{ xPMIN.UD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMIN.UD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMIN.UD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMIN.UD_abs%d",k);
        ck(nm,[&]{ xPMIN.UD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xd5};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.LW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.LW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.LW_abs%d",k);
        ck(nm,[&]{ xPMUL.LW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xe5};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HW_abs%d",k);
        ck(nm,[&]{ xPMUL.HW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xe4};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HUW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HUW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HUW_abs%d",k);
        ck(nm,[&]{ xPMUL.HUW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xf4};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.UDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.UDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.UDQ_abs%d",k);
        ck(nm,[&]{ xPMUL.UDQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x0b38};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.HRSW x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.HRSW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.HRSW_abs%d",k);
        ck(nm,[&]{ xPMUL.HRSW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x4038};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.LD x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.LD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.LD_abs%d",k);
        ck(nm,[&]{ xPMUL.LD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x2838};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PMUL.DQ x%d,x%d",a,b);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PMUL.DQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PMUL.DQ_abs%d",k);
        ck(nm,[&]{ xPMUL.DQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xeb};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"POR x%d,x%d",a,b);
        ck(nm,[&]{ xPOR(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"POR_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPOR(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"POR_abs%d",k);
        ck(nm,[&]{ xPOR(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegImmSSE sh={0x66,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.D x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.D(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.D_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.D(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0xf2,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.LW x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.LW(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.LW_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.LW(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegImmSSE sh={0xf3,0x70};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PSHUF.HW x%d,x%d i%d",a,b,i);
        ck(nm,[&]{ xPSHUF.HW(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b),IM8[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int i=0;i<5;i++){
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(1),SC[s],DSP[bs%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.HW_m s%d b%d i%d",SC[s],bs,i);
        ck(nm,[&]{ xPSHUF.HW(xRegisterSSE(3),M(),IM8[i]); },[&]{ sh(xRegisterSSE(3),M(),IM8[i]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x0038};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PSHUF.B x%d,x%d",a,b);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PSHUF.B_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PSHUF.B_abs%d",k);
        ck(nm,[&]{ xPSHUF.B(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x1738};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PTEST x%d,x%d",a,b);
        ck(nm,[&]{ xPTEST(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PTEST_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPTEST(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PTEST_abs%d",k);
        ck(nm,[&]{ xPTEST(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x60};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LBW x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LBW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LBW_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LBW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x61};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LWD x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LWD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LWD_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LWD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x62};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LDQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6c};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.LQDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x68};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HBW x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HBW_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HBW_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x69};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HWD x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HWD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HWD_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HWD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6a};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HDQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x6d};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ x%d,x%d",a,b);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PUNPCK.HQDQ_abs%d",k);
        ck(nm,[&]{ xPUNPCK.HQDQ(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0xef};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"PXOR x%d,x%d",a,b);
        ck(nm,[&]{ xPXOR(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"PXOR_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xPXOR(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"PXOR_abs%d",k);
        ck(nm,[&]{ xPXOR(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x51};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SQRT.PS x%d,x%d",a,b);
        ck(nm,[&]{ xSQRT.PS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"SQRT.PS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSQRT.PS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"SQRT.PS_abs%d",k);
        ck(nm,[&]{ xSQRT.PS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf3,0x51};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SQRT.SS x%d,x%d",a,b);
        ck(nm,[&]{ xSQRT.SS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"SQRT.SS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSQRT.SS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"SQRT.SS_abs%d",k);
        ck(nm,[&]{ xSQRT.SS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0xf2,0x51};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SQRT.SD x%d,x%d",a,b);
        ck(nm,[&]{ xSQRT.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"SQRT.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSQRT.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"SQRT.SD_abs%d",k);
        ck(nm,[&]{ xSQRT.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x2e};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UCOMI.SS x%d,x%d",a,b);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UCOMI.SS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UCOMI.SS_abs%d",k);
        ck(nm,[&]{ xUCOMI.SS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x2e};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UCOMI.SD x%d,x%d",a,b);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UCOMI.SD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UCOMI.SD_abs%d",k);
        ck(nm,[&]{ xUCOMI.SD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x15};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UNPCK.HPS x%d,x%d",a,b);
        ck(nm,[&]{ xUNPCK.HPS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UNPCK.HPS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUNPCK.HPS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UNPCK.HPS_abs%d",k);
        ck(nm,[&]{ xUNPCK.HPS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x15};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UNPCK.HPD x%d,x%d",a,b);
        ck(nm,[&]{ xUNPCK.HPD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UNPCK.HPD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUNPCK.HPD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UNPCK.HPD_abs%d",k);
        ck(nm,[&]{ xUNPCK.HPD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x00,0x14};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UNPCK.LPS x%d,x%d",a,b);
        ck(nm,[&]{ xUNPCK.LPS(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UNPCK.LPS_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUNPCK.LPS(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UNPCK.LPS_abs%d",k);
        ck(nm,[&]{ xUNPCK.LPS(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    { const shim_SimdRegSSE sh={0x66,0x14};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"UNPCK.LPD x%d,x%d",a,b);
        ck(nm,[&]{ xUNPCK.LPD(xRegisterSSE(a),xRegisterSSE(b)); },[&]{ sh(xRegisterSSE(a),xRegisterSSE(b)); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr128[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%5])]; };
        snprintf(nm,sizeof nm,"UNPCK.LPD_m s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xUNPCK.LPD(xRegisterSSE(3),M()); },[&]{ sh(xRegisterSSE(3),M()); }); }
      for(int k=0;k<3;k++){
        snprintf(nm,sizeof nm,"UNPCK.LPD_abs%d",k);
        ck(nm,[&]{ xUNPCK.LPD(xRegisterSSE(5),ptr128[ABS[k]]); },[&]{ sh(xRegisterSSE(5),ptr128[ABS[k]]); }); } }
    printf("sse: cases %ld | divergent %ld  (76 members, 8 imm8)\n",C,F);
    return F?1:0; }
