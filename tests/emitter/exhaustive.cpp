/* Exhaustive emitter comparison, generated. Operand size is the outer loop
 * because that is the dimension a hand-written matrix drops -- it is what was
 * missing when the switched build black-screened. Every family is driven at
 * every size, over the full register file (including the four 8-bit ids that
 * carry the 0x10 REX marker), every addressing shape, every scale, and
 * immediates on both sides of the s8 boundary. */
#include "common/emitter/x86emitter.h"
#include "tests/emitter/reference/x86emitter_shim.h"
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
using namespace x86Emitter;
static u8* B; static long C=0,F=0; static char FIRST[512]; static int NF=0;
template <typename A,typename Bf> static void ck(const char* w,A a,Bf b){
    u8 x[40],y[40]; size_t nx,ny;
    x86Ptr = (u8*)(B); a(); nx=x86Ptr-B; if(nx>40)nx=40; memcpy(x,B,nx);
    memset(B,0xcc,40); x86Ptr = (u8*)(B); b(); ny=x86Ptr-B; if(ny>40)ny=40; memcpy(y,B,ny);
    C++;
    if(nx!=ny||memcmp(x,y,nx)){ F++;
        if(NF<12){ int o=snprintf(FIRST,sizeof FIRST,"%-30s ref[%zu]:",w,nx);
            for(size_t i=0;i<nx;i++)o+=snprintf(FIRST+o,sizeof(FIRST)-o," %02x",x[i]);
            o+=snprintf(FIRST+o,sizeof(FIRST)-o,"  c89[%zu]:",ny);
            for(size_t i=0;i<ny;i++)o+=snprintf(FIRST+o,sizeof(FIRST)-o," %02x",y[i]);
            printf("  %s\n",FIRST); NF++; } } }
static const int ID8[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x14,0x15,0x16,0x17};
static const s32 IMM[10]={0,1,2,0x7f,0x80,-1,-2,-0x80,-0x81,0x12345};
static const int SC[4]={1,2,4,8};
static const s32 DSP[6]={0,1,0x7f,0x80,-1,-0x1000};
int main(){
    B=(u8*)mmap((void*)0x200000000ull,1<<20,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    void* const ABS[3]={(void*)0x200001000ull,(void*)0x410000ull,(void*)0x1fff0000ull};
    char nm[160];
    { const shim_Group1 sh={G1Type_ADD};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"ADD1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADD(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADD(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADD1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"ADD1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"ADD1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD1_abs%d i%d",k,i);
        ck(nm,[&]{ xADD(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADD};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADD2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADD(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADD(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADD2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"ADD2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"ADD2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD2_abs%d i%d",k,i);
        ck(nm,[&]{ xADD(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADD};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADD4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADD(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADD(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADD4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"ADD4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"ADD4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD4_abs%d i%d",k,i);
        ck(nm,[&]{ xADD(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADD};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADD8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADD(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADD(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADD8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"ADD8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"ADD8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADD(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADD8_abs%d i%d",k,i);
        ck(nm,[&]{ xADD(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_OR};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"OR1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xOR(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xOR(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"OR1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"OR1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"OR1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR1_abs%d i%d",k,i);
        ck(nm,[&]{ xOR(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_OR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"OR2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xOR(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xOR(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"OR2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"OR2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"OR2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR2_abs%d i%d",k,i);
        ck(nm,[&]{ xOR(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_OR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"OR4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xOR(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xOR(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"OR4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"OR4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"OR4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR4_abs%d i%d",k,i);
        ck(nm,[&]{ xOR(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_OR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"OR8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xOR(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xOR(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"OR8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"OR8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"OR8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"OR8_abs%d i%d",k,i);
        ck(nm,[&]{ xOR(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_AND};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"AND1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xAND(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xAND(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"AND1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"AND1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"AND1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND1_abs%d i%d",k,i);
        ck(nm,[&]{ xAND(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_AND};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"AND2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xAND(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xAND(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"AND2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"AND2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"AND2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND2_abs%d i%d",k,i);
        ck(nm,[&]{ xAND(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_AND};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"AND4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xAND(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xAND(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"AND4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"AND4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"AND4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND4_abs%d i%d",k,i);
        ck(nm,[&]{ xAND(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_AND};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"AND8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xAND(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xAND(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"AND8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"AND8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"AND8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xAND(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"AND8_abs%d i%d",k,i);
        ck(nm,[&]{ xAND(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_SUB};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"SUB1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xSUB(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xSUB(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"SUB1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"SUB1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"SUB1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB1_abs%d i%d",k,i);
        ck(nm,[&]{ xSUB(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_SUB};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SUB2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xSUB(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xSUB(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"SUB2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"SUB2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"SUB2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB2_abs%d i%d",k,i);
        ck(nm,[&]{ xSUB(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_SUB};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SUB4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xSUB(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xSUB(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"SUB4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"SUB4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"SUB4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB4_abs%d i%d",k,i);
        ck(nm,[&]{ xSUB(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_SUB};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"SUB8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xSUB(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xSUB(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"SUB8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"SUB8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"SUB8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xSUB(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"SUB8_abs%d i%d",k,i);
        ck(nm,[&]{ xSUB(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_XOR};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"XOR1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xXOR(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xXOR(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"XOR1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"XOR1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"XOR1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR1_abs%d i%d",k,i);
        ck(nm,[&]{ xXOR(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_XOR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"XOR2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xXOR(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xXOR(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"XOR2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"XOR2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"XOR2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR2_abs%d i%d",k,i);
        ck(nm,[&]{ xXOR(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_XOR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"XOR4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xXOR(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xXOR(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"XOR4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"XOR4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"XOR4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR4_abs%d i%d",k,i);
        ck(nm,[&]{ xXOR(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_XOR};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"XOR8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xXOR(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xXOR(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"XOR8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"XOR8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"XOR8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xXOR(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"XOR8_abs%d i%d",k,i);
        ck(nm,[&]{ xXOR(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_CMP};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"CMP1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xCMP(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xCMP(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"CMP1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"CMP1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"CMP1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP1_abs%d i%d",k,i);
        ck(nm,[&]{ xCMP(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_CMP};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"CMP2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xCMP(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xCMP(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"CMP2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"CMP2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"CMP2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP2_abs%d i%d",k,i);
        ck(nm,[&]{ xCMP(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_CMP};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"CMP4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xCMP(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xCMP(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"CMP4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"CMP4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"CMP4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP4_abs%d i%d",k,i);
        ck(nm,[&]{ xCMP(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_CMP};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"CMP8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xCMP(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xCMP(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"CMP8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"CMP8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"CMP8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xCMP(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"CMP8_abs%d i%d",k,i);
        ck(nm,[&]{ xCMP(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADC};
      for(int a=0;a<20;a++) for(int b=0;b<20;b++){
        snprintf(nm,sizeof nm,"ADC1_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADC(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sh(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
      for(int a=0;a<20;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC1_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADC(xRegister8(ID8[a]),IMM[i]); },[&]{ sh(xRegister8(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADC1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(xRegister8(1),M()); },[&]{ sh(xRegister8(1),M()); });
        snprintf(nm,sizeof nm,"ADC1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),xRegister8(1)); },[&]{ sh(M(),xRegister8(1)); });
        snprintf(nm,sizeof nm,"ADC1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC1_abs%d i%d",k,i);
        ck(nm,[&]{ xADC(ptr8[ABS[k]],IMM[i]); },[&]{ sh(ptr8[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADC};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADC2_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADC(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sh(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC2_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADC(xRegister16(ID8[a]),IMM[i]); },[&]{ sh(xRegister16(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADC2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(xRegister16(1),M()); },[&]{ sh(xRegister16(1),M()); });
        snprintf(nm,sizeof nm,"ADC2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),xRegister16(1)); },[&]{ sh(M(),xRegister16(1)); });
        snprintf(nm,sizeof nm,"ADC2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC2_abs%d i%d",k,i);
        ck(nm,[&]{ xADC(ptr16[ABS[k]],IMM[i]); },[&]{ sh(ptr16[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADC};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADC4_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADC(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sh(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC4_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADC(xRegister32(ID8[a]),IMM[i]); },[&]{ sh(xRegister32(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADC4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(xRegister32(1),M()); },[&]{ sh(xRegister32(1),M()); });
        snprintf(nm,sizeof nm,"ADC4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),xRegister32(1)); },[&]{ sh(M(),xRegister32(1)); });
        snprintf(nm,sizeof nm,"ADC4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC4_abs%d i%d",k,i);
        ck(nm,[&]{ xADC(ptr32[ABS[k]],IMM[i]); },[&]{ sh(ptr32[ABS[k]],IMM[i]); }); } }
    { const shim_Group1 sh={G1Type_ADC};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"ADC8_rr %x %x",ID8[a],ID8[b]);
        ck(nm,[&]{ xADC(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sh(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
      for(int a=0;a<16;a++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC8_ri %x i%d",ID8[a],i);
        ck(nm,[&]{ xADC(xRegister64(ID8[a]),IMM[i]); },[&]{ sh(xRegister64(ID8[a]),IMM[i]); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs+ix)%6])]; };
        snprintf(nm,sizeof nm,"ADC8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(xRegister64(1),M()); },[&]{ sh(xRegister64(1),M()); });
        snprintf(nm,sizeof nm,"ADC8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),xRegister64(1)); },[&]{ sh(M(),xRegister64(1)); });
        snprintf(nm,sizeof nm,"ADC8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xADC(M(),IMM[(bs+ix)%10]); },[&]{ sh(M(),IMM[(bs+ix)%10]); }); }
      for(int k=0;k<3;k++) for(int i=0;i<10;i++){
        snprintf(nm,sizeof nm,"ADC8_abs%d i%d",k,i);
        ck(nm,[&]{ xADC(ptr64[ABS[k]],IMM[i]); },[&]{ sh(ptr64[ABS[k]],IMM[i]); }); } }
    { const shim_Group2 sh={G2Type_ROL};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROL1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROL(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROL1_cl %x",ID8[a]);
        ck(nm,[&]{ xROL(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROL2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROL(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROL2_cl %x",ID8[a]);
        ck(nm,[&]{ xROL(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROL4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROL(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROL4_cl %x",ID8[a]);
        ck(nm,[&]{ xROL(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROL8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROL(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROL8_cl %x",ID8[a]);
        ck(nm,[&]{ xROL(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROR};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROR1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROR(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROR1_cl %x",ID8[a]);
        ck(nm,[&]{ xROR(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROR2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROR(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROR2_cl %x",ID8[a]);
        ck(nm,[&]{ xROR(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROR4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROR(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROR4_cl %x",ID8[a]);
        ck(nm,[&]{ xROR(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_ROR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"ROR8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xROR(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"ROR8_cl %x",ID8[a]);
        ck(nm,[&]{ xROR(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCL};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCL1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCL(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCL1_cl %x",ID8[a]);
        ck(nm,[&]{ xRCL(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCL2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCL(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCL2_cl %x",ID8[a]);
        ck(nm,[&]{ xRCL(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCL4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCL(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCL4_cl %x",ID8[a]);
        ck(nm,[&]{ xRCL(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCL8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCL(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCL8_cl %x",ID8[a]);
        ck(nm,[&]{ xRCL(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCR};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCR1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCR(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCR1_cl %x",ID8[a]);
        ck(nm,[&]{ xRCR(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCR2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCR(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCR2_cl %x",ID8[a]);
        ck(nm,[&]{ xRCR(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCR4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCR(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCR4_cl %x",ID8[a]);
        ck(nm,[&]{ xRCR(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_RCR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"RCR8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xRCR(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"RCR8_cl %x",ID8[a]);
        ck(nm,[&]{ xRCR(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHL};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHL1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHL(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHL1_cl %x",ID8[a]);
        ck(nm,[&]{ xSHL(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHL2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHL(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHL2_cl %x",ID8[a]);
        ck(nm,[&]{ xSHL(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHL4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHL(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHL4_cl %x",ID8[a]);
        ck(nm,[&]{ xSHL(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHL};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHL8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHL(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHL8_cl %x",ID8[a]);
        ck(nm,[&]{ xSHL(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHR};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHR1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHR(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHR1_cl %x",ID8[a]);
        ck(nm,[&]{ xSHR(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHR2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHR(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHR2_cl %x",ID8[a]);
        ck(nm,[&]{ xSHR(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHR4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHR(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHR4_cl %x",ID8[a]);
        ck(nm,[&]{ xSHR(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SHR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SHR8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSHR(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SHR8_cl %x",ID8[a]);
        ck(nm,[&]{ xSHR(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SAR};
      for(int a=0;a<20;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SAR1 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSAR(xRegister8(ID8[a]),(u8)c); },[&]{ sh(xRegister8(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SAR1_cl %x",ID8[a]);
        ck(nm,[&]{ xSAR(xRegister8(ID8[a]),cl); },[&]{ sh(xRegister8(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SAR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SAR2 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSAR(xRegister16(ID8[a]),(u8)c); },[&]{ sh(xRegister16(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SAR2_cl %x",ID8[a]);
        ck(nm,[&]{ xSAR(xRegister16(ID8[a]),cl); },[&]{ sh(xRegister16(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SAR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SAR4 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSAR(xRegister32(ID8[a]),(u8)c); },[&]{ sh(xRegister32(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SAR4_cl %x",ID8[a]);
        ck(nm,[&]{ xSAR(xRegister32(ID8[a]),cl); },[&]{ sh(xRegister32(ID8[a]),cl); }); } }
    { const shim_Group2 sh={G2Type_SAR};
      for(int a=0;a<16;a++){ for(int c=0;c<34;c++){
        snprintf(nm,sizeof nm,"SAR8 %x c%d",ID8[a],c);
        ck(nm,[&]{ xSAR(xRegister64(ID8[a]),(u8)c); },[&]{ sh(xRegister64(ID8[a]),(u8)c); }); }
        snprintf(nm,sizeof nm,"SAR8_cl %x",ID8[a]);
        ck(nm,[&]{ xSAR(xRegister64(ID8[a]),cl); },[&]{ sh(xRegister64(ID8[a]),cl); }); } }
    { const shim_Group3 sh={G3Type_NOT};
      for(int a=0;a<20;a++){
        snprintf(nm,sizeof nm,"NOT1 %x",ID8[a]);
        ck(nm,[&]{ xNOT(xRegister8(ID8[a])); },[&]{ sh(xRegister8(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NOT1_m b%d",bs);
        ck(nm,[&]{ xNOT(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NOT};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NOT2 %x",ID8[a]);
        ck(nm,[&]{ xNOT(xRegister16(ID8[a])); },[&]{ sh(xRegister16(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NOT2_m b%d",bs);
        ck(nm,[&]{ xNOT(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NOT};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NOT4 %x",ID8[a]);
        ck(nm,[&]{ xNOT(xRegister32(ID8[a])); },[&]{ sh(xRegister32(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NOT4_m b%d",bs);
        ck(nm,[&]{ xNOT(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NOT};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NOT8 %x",ID8[a]);
        ck(nm,[&]{ xNOT(xRegister64(ID8[a])); },[&]{ sh(xRegister64(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NOT8_m b%d",bs);
        ck(nm,[&]{ xNOT(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NEG};
      for(int a=0;a<20;a++){
        snprintf(nm,sizeof nm,"NEG1 %x",ID8[a]);
        ck(nm,[&]{ xNEG(xRegister8(ID8[a])); },[&]{ sh(xRegister8(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NEG1_m b%d",bs);
        ck(nm,[&]{ xNEG(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NEG};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NEG2 %x",ID8[a]);
        ck(nm,[&]{ xNEG(xRegister16(ID8[a])); },[&]{ sh(xRegister16(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NEG2_m b%d",bs);
        ck(nm,[&]{ xNEG(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NEG};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NEG4 %x",ID8[a]);
        ck(nm,[&]{ xNEG(xRegister32(ID8[a])); },[&]{ sh(xRegister32(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NEG4_m b%d",bs);
        ck(nm,[&]{ xNEG(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_NEG};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"NEG8 %x",ID8[a]);
        ck(nm,[&]{ xNEG(xRegister64(ID8[a])); },[&]{ sh(xRegister64(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"NEG8_m b%d",bs);
        ck(nm,[&]{ xNEG(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_MUL};
      for(int a=0;a<20;a++){
        snprintf(nm,sizeof nm,"UMUL1 %x",ID8[a]);
        ck(nm,[&]{ xUMUL(xRegister8(ID8[a])); },[&]{ sh(xRegister8(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UMUL1_m b%d",bs);
        ck(nm,[&]{ xUMUL(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_MUL};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UMUL2 %x",ID8[a]);
        ck(nm,[&]{ xUMUL(xRegister16(ID8[a])); },[&]{ sh(xRegister16(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UMUL2_m b%d",bs);
        ck(nm,[&]{ xUMUL(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_MUL};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UMUL4 %x",ID8[a]);
        ck(nm,[&]{ xUMUL(xRegister32(ID8[a])); },[&]{ sh(xRegister32(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UMUL4_m b%d",bs);
        ck(nm,[&]{ xUMUL(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_MUL};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UMUL8 %x",ID8[a]);
        ck(nm,[&]{ xUMUL(xRegister64(ID8[a])); },[&]{ sh(xRegister64(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UMUL8_m b%d",bs);
        ck(nm,[&]{ xUMUL(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_DIV};
      for(int a=0;a<20;a++){
        snprintf(nm,sizeof nm,"UDIV1 %x",ID8[a]);
        ck(nm,[&]{ xUDIV(xRegister8(ID8[a])); },[&]{ sh(xRegister8(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UDIV1_m b%d",bs);
        ck(nm,[&]{ xUDIV(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_DIV};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UDIV2 %x",ID8[a]);
        ck(nm,[&]{ xUDIV(xRegister16(ID8[a])); },[&]{ sh(xRegister16(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UDIV2_m b%d",bs);
        ck(nm,[&]{ xUDIV(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_DIV};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UDIV4 %x",ID8[a]);
        ck(nm,[&]{ xUDIV(xRegister32(ID8[a])); },[&]{ sh(xRegister32(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UDIV4_m b%d",bs);
        ck(nm,[&]{ xUDIV(M()); },[&]{ sh(M()); }); } }
    { const shim_Group3 sh={G3Type_DIV};
      for(int a=0;a<16;a++){
        snprintf(nm,sizeof nm,"UDIV8 %x",ID8[a]);
        ck(nm,[&]{ xUDIV(xRegister64(ID8[a])); },[&]{ sh(xRegister64(ID8[a])); }); }
      for(int bs=0;bs<16;bs++){
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(3),4,0x20)]; };
        snprintf(nm,sizeof nm,"UDIV8_m b%d",bs);
        ck(nm,[&]{ xUDIV(M()); },[&]{ sh(M()); }); } }
    { const shim_Mov sM; const shim_Test sT;
      const shim_IncDec sI={false}, sD={true};
      for(int a=0;a<20;a++){
        for(int b=0;b<20;b++){
          snprintf(nm,sizeof nm,"MOV1_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xMOV(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sM(xRegister8(ID8[a]),xRegister8(ID8[b])); });
          snprintf(nm,sizeof nm,"TEST1_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xTEST(xRegister8(ID8[a]),xRegister8(ID8[b])); },[&]{ sT(xRegister8(ID8[a]),xRegister8(ID8[b])); }); }
        for(int i=0;i<10;i++){
          snprintf(nm,sizeof nm,"MOV1_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister8(ID8[a]),(sptr)IMM[i]); },[&]{ sM(xRegister8(ID8[a]),(sptr)IMM[i]); });
          snprintf(nm,sizeof nm,"MOV1_rip %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister8(ID8[a]),(sptr)IMM[i],true); },[&]{ sM(xRegister8(ID8[a]),(sptr)IMM[i],true); });
          snprintf(nm,sizeof nm,"TEST1_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xTEST(xRegister8(ID8[a]),IMM[i]); },[&]{ sT(xRegister8(ID8[a]),IMM[i]); }); }
        snprintf(nm,sizeof nm,"INC1 %x",ID8[a]);
        ck(nm,[&]{ xINC(xRegister8(ID8[a])); },[&]{ sI(xRegister8(ID8[a])); });
        snprintf(nm,sizeof nm,"DEC1 %x",ID8[a]);
        ck(nm,[&]{ xDEC(xRegister8(ID8[a])); },[&]{ sD(xRegister8(ID8[a])); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr8[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs*3+ix)%6])]; };
        snprintf(nm,sizeof nm,"MOV1_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(xRegister8(2),M()); },[&]{ sM(xRegister8(2),M()); });
        snprintf(nm,sizeof nm,"MOV1_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),xRegister8(2)); },[&]{ sM(M(),xRegister8(2)); });
        snprintf(nm,sizeof nm,"MOV1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),(sptr)IMM[(bs+ix)%10]); },[&]{ sM(M(),(sptr)IMM[(bs+ix)%10]); });
        snprintf(nm,sizeof nm,"TEST1_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xTEST(M(),IMM[(bs+ix)%10]); },[&]{ sT(M(),IMM[(bs+ix)%10]); }); } }
    { const shim_Mov sM; const shim_Test sT;
      const shim_IncDec sI={false}, sD={true};
      for(int a=0;a<16;a++){
        for(int b=0;b<16;b++){
          snprintf(nm,sizeof nm,"MOV2_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xMOV(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sM(xRegister16(ID8[a]),xRegister16(ID8[b])); });
          snprintf(nm,sizeof nm,"TEST2_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xTEST(xRegister16(ID8[a]),xRegister16(ID8[b])); },[&]{ sT(xRegister16(ID8[a]),xRegister16(ID8[b])); }); }
        for(int i=0;i<10;i++){
          snprintf(nm,sizeof nm,"MOV2_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister16(ID8[a]),(sptr)IMM[i]); },[&]{ sM(xRegister16(ID8[a]),(sptr)IMM[i]); });
          snprintf(nm,sizeof nm,"MOV2_rip %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister16(ID8[a]),(sptr)IMM[i],true); },[&]{ sM(xRegister16(ID8[a]),(sptr)IMM[i],true); });
          snprintf(nm,sizeof nm,"TEST2_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xTEST(xRegister16(ID8[a]),IMM[i]); },[&]{ sT(xRegister16(ID8[a]),IMM[i]); }); }
        snprintf(nm,sizeof nm,"INC2 %x",ID8[a]);
        ck(nm,[&]{ xINC(xRegister16(ID8[a])); },[&]{ sI(xRegister16(ID8[a])); });
        snprintf(nm,sizeof nm,"DEC2 %x",ID8[a]);
        ck(nm,[&]{ xDEC(xRegister16(ID8[a])); },[&]{ sD(xRegister16(ID8[a])); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr16[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs*3+ix)%6])]; };
        snprintf(nm,sizeof nm,"MOV2_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(xRegister16(2),M()); },[&]{ sM(xRegister16(2),M()); });
        snprintf(nm,sizeof nm,"MOV2_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),xRegister16(2)); },[&]{ sM(M(),xRegister16(2)); });
        snprintf(nm,sizeof nm,"MOV2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),(sptr)IMM[(bs+ix)%10]); },[&]{ sM(M(),(sptr)IMM[(bs+ix)%10]); });
        snprintf(nm,sizeof nm,"TEST2_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xTEST(M(),IMM[(bs+ix)%10]); },[&]{ sT(M(),IMM[(bs+ix)%10]); }); } }
    { const shim_Mov sM; const shim_Test sT;
      const shim_IncDec sI={false}, sD={true};
      for(int a=0;a<16;a++){
        for(int b=0;b<16;b++){
          snprintf(nm,sizeof nm,"MOV4_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xMOV(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sM(xRegister32(ID8[a]),xRegister32(ID8[b])); });
          snprintf(nm,sizeof nm,"TEST4_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xTEST(xRegister32(ID8[a]),xRegister32(ID8[b])); },[&]{ sT(xRegister32(ID8[a]),xRegister32(ID8[b])); }); }
        for(int i=0;i<10;i++){
          snprintf(nm,sizeof nm,"MOV4_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister32(ID8[a]),(sptr)IMM[i]); },[&]{ sM(xRegister32(ID8[a]),(sptr)IMM[i]); });
          snprintf(nm,sizeof nm,"MOV4_rip %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister32(ID8[a]),(sptr)IMM[i],true); },[&]{ sM(xRegister32(ID8[a]),(sptr)IMM[i],true); });
          snprintf(nm,sizeof nm,"TEST4_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xTEST(xRegister32(ID8[a]),IMM[i]); },[&]{ sT(xRegister32(ID8[a]),IMM[i]); }); }
        snprintf(nm,sizeof nm,"INC4 %x",ID8[a]);
        ck(nm,[&]{ xINC(xRegister32(ID8[a])); },[&]{ sI(xRegister32(ID8[a])); });
        snprintf(nm,sizeof nm,"DEC4 %x",ID8[a]);
        ck(nm,[&]{ xDEC(xRegister32(ID8[a])); },[&]{ sD(xRegister32(ID8[a])); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs*3+ix)%6])]; };
        snprintf(nm,sizeof nm,"MOV4_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(xRegister32(2),M()); },[&]{ sM(xRegister32(2),M()); });
        snprintf(nm,sizeof nm,"MOV4_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),xRegister32(2)); },[&]{ sM(M(),xRegister32(2)); });
        snprintf(nm,sizeof nm,"MOV4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),(sptr)IMM[(bs+ix)%10]); },[&]{ sM(M(),(sptr)IMM[(bs+ix)%10]); });
        snprintf(nm,sizeof nm,"TEST4_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xTEST(M(),IMM[(bs+ix)%10]); },[&]{ sT(M(),IMM[(bs+ix)%10]); }); } }
    { const shim_Mov sM; const shim_Test sT;
      const shim_IncDec sI={false}, sD={true};
      for(int a=0;a<16;a++){
        for(int b=0;b<16;b++){
          snprintf(nm,sizeof nm,"MOV8_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xMOV(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sM(xRegister64(ID8[a]),xRegister64(ID8[b])); });
          snprintf(nm,sizeof nm,"TEST8_rr %x %x",ID8[a],ID8[b]);
          ck(nm,[&]{ xTEST(xRegister64(ID8[a]),xRegister64(ID8[b])); },[&]{ sT(xRegister64(ID8[a]),xRegister64(ID8[b])); }); }
        for(int i=0;i<10;i++){
          snprintf(nm,sizeof nm,"MOV8_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister64(ID8[a]),(sptr)IMM[i]); },[&]{ sM(xRegister64(ID8[a]),(sptr)IMM[i]); });
          snprintf(nm,sizeof nm,"MOV8_rip %x i%d",ID8[a],i);
          ck(nm,[&]{ xMOV(xRegister64(ID8[a]),(sptr)IMM[i],true); },[&]{ sM(xRegister64(ID8[a]),(sptr)IMM[i],true); });
          snprintf(nm,sizeof nm,"TEST8_ri %x i%d",ID8[a],i);
          ck(nm,[&]{ xTEST(xRegister64(ID8[a]),IMM[i]); },[&]{ sT(xRegister64(ID8[a]),IMM[i]); }); }
        snprintf(nm,sizeof nm,"INC8 %x",ID8[a]);
        ck(nm,[&]{ xINC(xRegister64(ID8[a])); },[&]{ sI(xRegister64(ID8[a])); });
        snprintf(nm,sizeof nm,"DEC8 %x",ID8[a]);
        ck(nm,[&]{ xDEC(xRegister64(ID8[a])); },[&]{ sD(xRegister64(ID8[a])); }); }
      for(int s=0;s<4;s++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
        auto M=[&]{ return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(ix),SC[s],DSP[(bs*3+ix)%6])]; };
        snprintf(nm,sizeof nm,"MOV8_rm s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(xRegister64(2),M()); },[&]{ sM(xRegister64(2),M()); });
        snprintf(nm,sizeof nm,"MOV8_mr s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),xRegister64(2)); },[&]{ sM(M(),xRegister64(2)); });
        snprintf(nm,sizeof nm,"MOV8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xMOV(M(),(sptr)IMM[(bs+ix)%10]); },[&]{ sM(M(),(sptr)IMM[(bs+ix)%10]); });
        snprintf(nm,sizeof nm,"TEST8_mi s%d b%d i%d",SC[s],bs,ix);
        ck(nm,[&]{ xTEST(M(),IMM[(bs+ix)%10]); },[&]{ sT(M(),IMM[(bs+ix)%10]); }); } }
    printf("exhaustive: cases %ld | divergent %ld\n",C,F);
    return F?1:0; }
