/* Oracle for the shim: same method as oracle.cpp, but the C89 side goes
 * through the C++ shim rather than calling the macros directly, so the
 * bridging (operand-type mapping, cursor store-back, width selection) is
 * verified too and not just the encoders underneath it. */
#include "common/emitter/x86emitter.h"
#include "common/emitter/x86emitter_shim.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <sys/mman.h>
using namespace x86Emitter;

static u8* g_buf; static long g_cases=0, g_fail=0;
static std::vector<std::string> g_f;

template <typename A, typename B>
static void check(const char* what, A a, B b)
{
    u8 x[32], y[32]; size_t nx, ny;
    xSetPtr(g_buf); a(); nx = xGetPtr()-g_buf; memcpy(x,g_buf,nx);
    memset(g_buf,0xcc,32);
    xSetPtr(g_buf); b(); ny = xGetPtr()-g_buf; memcpy(y,g_buf,ny);
    g_cases++;
    if (nx!=ny || memcmp(x,y,nx)) {
        g_fail++;
        if (g_f.size()<20) {
            char l[400]; int o=snprintf(l,sizeof l,"%-34s ref[%zu]:",what,nx);
            for(size_t i=0;i<nx&&o<200;i++) o+=snprintf(l+o,sizeof(l)-o," %02x",x[i]);
            o+=snprintf(l+o,sizeof(l)-o,"  shim[%zu]:",ny);
            for(size_t i=0;i<ny&&o<380;i++) o+=snprintf(l+o,sizeof(l)-o," %02x",y[i]);
            g_f.push_back(l);
        }
    }
}

int main()
{
    g_buf=(u8*)mmap((void*)0x200000000ull,1<<20,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    static u64 mem[8];
    const void* addrs[]={(const void*)0x200001000ull,(const void*)0x40f000ull,mem};
    const int naddr=3;
    const s32 imms[]={0,1,0x7f,0x80,-1,-0x80,-0x81,0x1fffff};
    const int nimm=8;
    char nm[128];

    struct G1 { G1Type t; const xImpl_Group1* ref; const char* n; };
    const shim_Group1 sADD{G1Type_ADD}, sOR{G1Type_OR}, sAND{G1Type_AND},
                      sSUB{G1Type_SUB}, sXOR{G1Type_XOR}, sCMP{G1Type_CMP};
    const shim_Group1* sg[6]={&sADD,&sOR,&sAND,&sSUB,&sXOR,&sCMP};
    const xImpl_Group1* rg[6]={&xADD,&xOR,&xAND,&xSUB,&xXOR,&xCMP};
    const char* gn[6]={"ADD","OR","AND","SUB","XOR","CMP"};

    /* group1 reg,reg and reg,imm at 32 and 64 bit */

    for(int g=0;g<6;g++)
    for(int w=0;w<=1;w++)
    for(int d=0;d<16;d++)
    for(int s=0;s<16;s++){
        snprintf(nm,sizeof nm,"%s_RR w%d d%d s%d",gn[g],w,d,s);
        check(nm,[&]{ if(w)(*rg[g])(xRegister64(d),xRegister64(s)); else (*rg[g])(xRegister32(d),xRegister32(s)); },
                 [&]{ if(w)(*sg[g])(xRegister64(d),xRegister64(s)); else (*sg[g])(xRegister32(d),xRegister32(s)); });
    }
    for(int g=0;g<6;g++)
    for(int w=0;w<=1;w++)
    for(int d=0;d<16;d++)
    for(int i=0;i<nimm;i++){
        snprintf(nm,sizeof nm,"%s_RI w%d d%d i%d",gn[g],w,d,i);
        check(nm,[&]{ if(w)(*rg[g])(xRegister64(d),imms[i]); else (*rg[g])(xRegister32(d),imms[i]); },
                 [&]{ if(w)(*sg[g])(xRegister64(d),imms[i]); else (*sg[g])(xRegister32(d),imms[i]); });
    }
    /* mov */
    const shim_Mov sMOV;
    for(int w=0;w<=1;w++) for(int d=0;d<16;d++) for(int s=0;s<16;s++){
        snprintf(nm,sizeof nm,"MOV_RR w%d d%d s%d",w,d,s);
        check(nm,[&]{ if(w)xMOV(xRegister64(d),xRegister64(s)); else xMOV(xRegister32(d),xRegister32(s)); },
                 [&]{ if(w)sMOV(xRegister64(d),xRegister64(s)); else sMOV(xRegister32(d),xRegister32(s)); });
    }
    for(int w=0;w<=1;w++) for(int d=0;d<16;d++) for(int a=0;a<naddr;a++){
        snprintf(nm,sizeof nm,"MOV_R_M w%d d%d a%d",w,d,a);
        check(nm,[&]{ if(w)xMOV(xRegister64(d),ptrNative[addrs[a]]); else xMOV(xRegister32(d),ptr32[addrs[a]]); },
                 [&]{ if(w)sMOV(xRegister64(d),ptrNative[addrs[a]]); else sMOV(xRegister32(d),ptr32[addrs[a]]); });
        snprintf(nm,sizeof nm,"MOV_M_R w%d d%d a%d",w,d,a);
        check(nm,[&]{ if(w)xMOV(ptrNative[addrs[a]],xRegister64(d)); else xMOV(ptr32[addrs[a]],xRegister32(d)); },
                 [&]{ if(w)sMOV(ptrNative[addrs[a]],xRegister64(d)); else sMOV(ptr32[addrs[a]],xRegister32(d)); });
    }
    /* group1 against memory (base+index) */
    for(int g=0;g<6;g++) for(int b=0;b<8;b++) for(int x2=0;x2<8;x2++){
        snprintf(nm,sizeof nm,"%s_R_MEM b%d x%d",gn[g],b,x2);
        auto mk=[&]{ return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(x2),4,0x30)]; };
        check(nm,[&]{ (*rg[g])(xRegister32(1),mk()); },
                 [&]{ (*sg[g])(xRegister32(1),mk()); });
    }
    /* shifts */
    const shim_Group2 sSHL{G2Type_SHL}, sSHR{G2Type_SHR}, sSAR{G2Type_SAR};
    for(int w=0;w<=1;w++) for(int r=0;r<16;r++) for(int s=0;s<33;s++){
        snprintf(nm,sizeof nm,"SHL w%d r%d s%d",w,r,s);
        check(nm,[&]{ if(w)xSHL(xRegister64(r),s); else xSHL(xRegister32(r),s); },
                 [&]{ if(w)sSHL(xRegister64(r),s); else sSHL(xRegister32(r),s); });
        snprintf(nm,sizeof nm,"SAR w%d r%d s%d",w,r,s);
        check(nm,[&]{ if(w)xSAR(xRegister64(r),s); else xSAR(xRegister32(r),s); },
                 [&]{ if(w)sSAR(xRegister64(r),s); else sSAR(xRegister32(r),s); });
    }
    /* SSE through the shim */
    const shim_SimdRegSSE sPXOR{0x66,0xef}, sMOVAPS{0x00,0x28}, sADDPS{0x00,0x58};
    for(int a2=0;a2<16;a2++) for(int b2=0;b2<16;b2++){
        snprintf(nm,sizeof nm,"PXOR x%d,x%d",a2,b2);
        check(nm,[&]{ xOpWrite0F(0x66,0xef,xRegisterSSE(a2),xRegisterSSE(b2)); },
                 [&]{ sPXOR(xRegisterSSE(a2),xRegisterSSE(b2)); });
        snprintf(nm,sizeof nm,"ADDPS x%d,x%d",a2,b2);
        check(nm,[&]{ xOpWrite0F(0x00,0x58,xRegisterSSE(a2),xRegisterSSE(b2)); },
                 [&]{ sADDPS(xRegisterSSE(a2),xRegisterSSE(b2)); });
    }
    for(int r=0;r<16;r++) for(int b=0;b<8;b++){
        snprintf(nm,sizeof nm,"MOVAPS_MEM x%d b%d",r,b);
        auto mk=[&]{ return ptr128[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x20)]; };
        check(nm,[&]{ xOpWrite0F(0x00,0x28,xRegisterSSE(r),mk()); },
                 [&]{ sMOVAPS(xRegisterSSE(r),mk()); });
    }
    /* push/pop/ret/nop */
    for(int r=0;r<16;r++){
        snprintf(nm,sizeof nm,"PUSH r%d",r);
        check(nm,[&]{ xPUSH(xRegister64(r)); },[&]{ shim_PUSH(xRegister64(r)); });
        snprintf(nm,sizeof nm,"POP r%d",r);
        check(nm,[&]{ xPOP(xRegister64(r)); },[&]{ shim_POP(xRegister64(r)); });
    }

    /* ---- SSE functor families ---- */
    {
        const shim_PShuffle sPSHUF = { {0x66,0x70}, {0xf2,0x70}, {0xf3,0x70}, {0x66,0x0038} };
        const shim_PCompare sPCMP  = { {0x66,0x74},{0x66,0x75},{0x66,0x76},
                                       {0x66,0x64},{0x66,0x65},{0x66,0x66} };
        const shim_Shift    sPSRL  = { {0x66,0xd1,0x71,2}, {0x66,0xd2,0x72,2}, {0x66,0xd3,0x73,2} };
        const shim_Shift    sPSLL  = { {0x66,0xf1,0x71,6}, {0x66,0xf2,0x72,6}, {0x66,0xf3,0x73,6} };
        const u8 imm8s[]={0x00,0x1b,0x55,0xaa,0xff};

        for(int a=0;a<16;a++) for(int b=0;b<16;b++){
            snprintf(nm,sizeof nm,"PSHUF.D x%d,x%d",a,b);
            check(nm,[&]{ xPSHUF.D(xRegisterSSE(a),xRegisterSSE(b),0x1b); },
                     [&]{ sPSHUF.D(xRegisterSSE(a),xRegisterSSE(b),0x1b); });
            snprintf(nm,sizeof nm,"PSHUF.LW x%d,x%d",a,b);
            check(nm,[&]{ xPSHUF.LW(xRegisterSSE(a),xRegisterSSE(b),0x55); },
                     [&]{ sPSHUF.LW(xRegisterSSE(a),xRegisterSSE(b),0x55); });
            snprintf(nm,sizeof nm,"PCMP.EQD x%d,x%d",a,b);
            check(nm,[&]{ xPCMP.EQD(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPCMP.EQD(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PCMP.GTW x%d,x%d",a,b);
            check(nm,[&]{ xPCMP.GTW(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPCMP.GTW(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PSRL.D x%d,x%d",a,b);
            check(nm,[&]{ xPSRL.D(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPSRL.D(xRegisterSSE(a),xRegisterSSE(b)); });
        }
        for(int a=0;a<16;a++) for(int k=0;k<5;k++){
            snprintf(nm,sizeof nm,"PSRL.W imm x%d i%d",a,k);
            check(nm,[&]{ xPSRL.W(xRegisterSSE(a),imm8s[k]); },
                     [&]{ sPSRL.W(xRegisterSSE(a),imm8s[k]); });
            snprintf(nm,sizeof nm,"PSLL.Q imm x%d i%d",a,k);
            check(nm,[&]{ xPSLL.Q(xRegisterSSE(a),imm8s[k]); },
                     [&]{ sPSLL.Q(xRegisterSSE(a),imm8s[k]); });
            snprintf(nm,sizeof nm,"PSRL.DQ x%d i%d",a,k);
            check(nm,[&]{ xPSRL.DQ(xRegisterSSE(a),imm8s[k]); },
                     [&]{ sPSRL.DQ(xRegisterSSE(a),imm8s[k]); });
        }
    }


    /* ---- SSE move families: alignment selection is the interesting part ---- */
    {
        const shim_MoveSSE sMOVAPS = {0x00, true};
        const shim_MoveSSE sMOVUPS = {0x00, false};
        const shim_MoveDQ  sMOVDQA = {true};
        const shim_MoveDQ  sMOVDQU = {false};
        const shim_MovExtend sMOVSX = {true}, sMOVZX = {false};
        /* displacements chosen to straddle the 16-byte alignment rule */
        const s32 disps[] = {0, 0x10, 0x20, 4, 8, 0x1f, 0x100, 0x104};
        for(int a=0;a<16;a++) for(int b=0;b<16;b++){
            snprintf(nm,sizeof nm,"MOVAPS_RR x%d,x%d",a,b);
            check(nm,[&]{ xMOVAPS(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sMOVAPS(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"MOVDQA_RR x%d,x%d",a,b);
            check(nm,[&]{ xMOVDQA(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sMOVDQA(xRegisterSSE(a),xRegisterSSE(b)); });
        }
        for(int r=0;r<16;r++) for(int d=0;d<8;d++){
            /* displacement-only: alignment rule applies */
            auto M=[&]{ return ptr128[(void*)(uptr)(0x200001000u + disps[d])]; };
            snprintf(nm,sizeof nm,"MOVAPS_LD x%d d%d",r,d);
            check(nm,[&]{ xMOVAPS(xRegisterSSE(r),M()); },[&]{ sMOVAPS(xRegisterSSE(r),M()); });
            snprintf(nm,sizeof nm,"MOVUPS_LD x%d d%d",r,d);
            check(nm,[&]{ xMOVUPS(xRegisterSSE(r),M()); },[&]{ sMOVUPS(xRegisterSSE(r),M()); });
            snprintf(nm,sizeof nm,"MOVAPS_ST x%d d%d",r,d);
            check(nm,[&]{ xMOVAPS(M(),xRegisterSSE(r)); },[&]{ sMOVAPS(M(),xRegisterSSE(r)); });
            snprintf(nm,sizeof nm,"MOVDQA_LD x%d d%d",r,d);
            check(nm,[&]{ xMOVDQA(xRegisterSSE(r),M()); },[&]{ sMOVDQA(xRegisterSSE(r),M()); });
            snprintf(nm,sizeof nm,"MOVDQU_ST x%d d%d",r,d);
            check(nm,[&]{ xMOVDQU(M(),xRegisterSSE(r)); },[&]{ sMOVDQU(M(),xRegisterSSE(r)); });
            /* base+index: never counts as aligned regardless of displacement */
            auto B=[&]{ return ptr128[xAddressVoid(xAddressReg(1),xAddressReg(2),4,disps[d])]; };
            snprintf(nm,sizeof nm,"MOVAPS_LDBX x%d d%d",r,d);
            check(nm,[&]{ xMOVAPS(xRegisterSSE(r),B()); },[&]{ sMOVAPS(xRegisterSSE(r),B()); });
            snprintf(nm,sizeof nm,"MOVDQA_LDBX x%d d%d",r,d);
            check(nm,[&]{ xMOVDQA(xRegisterSSE(r),B()); },[&]{ sMOVDQA(xRegisterSSE(r),B()); });
        }
        for(int d=0;d<16;d++) for(int s2=0;s2<16;s2++){
            if(s2>=4&&s2<8) continue;
            snprintf(nm,sizeof nm,"MOVSX32_8 d%d s%d",d,s2);
            check(nm,[&]{ xMOVSX(xRegister32(d),xRegister8(s2)); },
                     [&]{ sMOVSX(xRegister32(d),xRegister8(s2)); });
            snprintf(nm,sizeof nm,"MOVZX32_16 d%d s%d",d,s2);
            check(nm,[&]{ xMOVZX(xRegister32(d),xRegister16(s2)); },
                     [&]{ sMOVZX(xRegister32(d),xRegister16(s2)); });
        }
    }


    /* ---- remaining aggregates, MOVSS/MOVSD, jumps and calls ---- */
    {
        const shim_AddSub sPADD = { {0x66,0xdc+0x20},{0x66,0xdc+0x21},{0x66,0xdc+0x22},{0x66,0xd4},
                                    {0x66,0xdc+0x10},{0x66,0xdc+0x11},{0x66,0xdc},{0x66,0xdc+1} };
        const shim_AddSub sPSUB = { {0x66,0xd8+0x20},{0x66,0xd8+0x21},{0x66,0xd8+0x22},{0x66,0xfb},
                                    {0x66,0xd8+0x10},{0x66,0xd8+0x11},{0x66,0xd8},{0x66,0xd8+1} };
        const shim_PUnpack sPUNPCK = { {0x66,0x60},{0x66,0x61},{0x66,0x62},{0x66,0x6c},
                                       {0x66,0x68},{0x66,0x69},{0x66,0x6a},{0x66,0x6d} };
        const shim_SimdRegSSE sPAND={0x66,0xdb}, sPOR={0x66,0xeb}, sPXOR={0x66,0xef};
        const shim_MovS sMOVSS={0xf3}, sMOVSD={0xf2};

        for(int a=0;a<16;a++) for(int b=0;b<16;b++){
            snprintf(nm,sizeof nm,"PADD.W x%d,x%d",a,b);
            check(nm,[&]{ xPADD.W(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPADD.W(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PADD.USB x%d,x%d",a,b);
            check(nm,[&]{ xPADD.USB(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPADD.USB(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PSUB.Q x%d,x%d",a,b);
            check(nm,[&]{ xPSUB.Q(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPSUB.Q(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PUNPCK.LQDQ x%d,x%d",a,b);
            check(nm,[&]{ xPUNPCK.LQDQ(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPUNPCK.LQDQ(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PUNPCK.HBW x%d,x%d",a,b);
            check(nm,[&]{ xPUNPCK.HBW(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPUNPCK.HBW(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PXOR x%d,x%d",a,b);
            check(nm,[&]{ xPXOR(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPXOR(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"PAND x%d,x%d",a,b);
            check(nm,[&]{ xPAND(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sPAND(xRegisterSSE(a),xRegisterSSE(b)); });
            snprintf(nm,sizeof nm,"MOVSS_RR x%d,x%d",a,b);
            check(nm,[&]{ xMOVSS(xRegisterSSE(a),xRegisterSSE(b)); },
                     [&]{ sMOVSS(xRegisterSSE(a),xRegisterSSE(b)); });
        }
        for(int r=0;r<16;r++) for(int a=0;a<naddr;a++){
            snprintf(nm,sizeof nm,"MOVSSZX x%d a%d",r,a);
            check(nm,[&]{ xMOVSSZX(xRegisterSSE(r),ptr32[addrs[a]]); },
                     [&]{ sMOVSS.ZX(xRegisterSSE(r),ptr32[addrs[a]]); });
            snprintf(nm,sizeof nm,"MOVSS_ST x%d a%d",r,a);
            check(nm,[&]{ xMOVSS(ptr32[addrs[a]],xRegisterSSE(r)); },
                     [&]{ sMOVSS(ptr32[addrs[a]],xRegisterSSE(r)); });
            snprintf(nm,sizeof nm,"MOVSDZX x%d a%d",r,a);
            check(nm,[&]{ xMOVSDZX(xRegisterSSE(r),ptr64[addrs[a]]); },
                     [&]{ sMOVSD.ZX(xRegisterSSE(r),ptr64[addrs[a]]); });
        }
        /* jump/call through a register */
        { const shim_JmpCall sJMP={true}, sCALL={false};
          for(int r=0;r<16;r++){
            snprintf(nm,sizeof nm,"JMP_R r%d",r);
            check(nm,[&]{ xJMP(xAddressReg(r)); },[&]{ sJMP(xAddressReg(r)); });
            snprintf(nm,sizeof nm,"CALL_R r%d",r);
            check(nm,[&]{ xCALL(xAddressReg(r)); },[&]{ sCALL(xAddressReg(r)); });
          } }
    }


    /* ---- the last families: SHUF, MOVD, iMul, FastCall ---- */
    {
        const shim_Shuffle sSHUF;
        const shim_iMul    sMUL;
        const u8 sels[]={0x00,0x1b,0x55,0xaa,0xff};
        for(int a=0;a<16;a++) for(int b=0;b<16;b++) for(int k=0;k<5;k+=2){
            snprintf(nm,sizeof nm,"SHUF.PS x%d,x%d s%d",a,b,k);
            check(nm,[&]{ xSHUF.PS(xRegisterSSE(a),xRegisterSSE(b),sels[k]); },
                     [&]{ sSHUF.PS(xRegisterSSE(a),xRegisterSSE(b),sels[k]); });
            /* PD masks the selector to two bits; PS does not */
            snprintf(nm,sizeof nm,"SHUF.PD x%d,x%d s%d",a,b,k);
            check(nm,[&]{ xSHUF.PD(xRegisterSSE(a),xRegisterSSE(b),sels[k]); },
                     [&]{ sSHUF.PD(xRegisterSSE(a),xRegisterSSE(b),sels[k]); });
        }
        for(int x=0;x<16;x++) for(int r=0;r<16;r++){
            snprintf(nm,sizeof nm,"MOVDZX x%d,r%d",x,r);
            check(nm,[&]{ xMOVDZX(xRegisterSSE(x),xRegister32(r)); },
                     [&]{ shim_MOVDZX(xRegisterSSE(x),xRegister32(r)); });
            snprintf(nm,sizeof nm,"MOVD r%d,x%d",r,x);
            check(nm,[&]{ xMOVD(xRegister32(r),xRegisterSSE(x)); },
                     [&]{ shim_MOVD(xRegister32(r),xRegisterSSE(x)); });
        }
        for(int x=0;x<16;x++) for(int a=0;a<naddr;a++){
            snprintf(nm,sizeof nm,"MOVDZX_M x%d a%d",x,a);
            check(nm,[&]{ xMOVDZX(xRegisterSSE(x),ptr32[addrs[a]]); },
                     [&]{ shim_MOVDZX(xRegisterSSE(x),ptr32[addrs[a]]); });
            snprintf(nm,sizeof nm,"MOVD_ST x%d a%d",x,a);
            check(nm,[&]{ xMOVD(ptr32[addrs[a]],xRegisterSSE(x)); },
                     [&]{ shim_MOVD(ptr32[addrs[a]],xRegisterSSE(x)); });
        }
        for(int d=0;d<16;d++) for(int s3=0;s3<16;s3++){
            snprintf(nm,sizeof nm,"IMUL2 d%d s%d",d,s3);
            check(nm,[&]{ xMUL(xRegister32(d),xRegister32(s3)); },
                     [&]{ sMUL(xRegister32(d),xRegister32(s3)); });
        }
        for(int d=0;d<16;d+=3) for(int s3=0;s3<16;s3+=3) for(int i=0;i<nimm;i++){
            snprintf(nm,sizeof nm,"IMUL3 d%d s%d i%d",d,s3,i);
            check(nm,[&]{ xMUL(xRegister32(d),xRegister32(s3),imms[i]); },
                     [&]{ sMUL(xRegister32(d),xRegister32(s3),imms[i]); });
        }
        /* xFastCall: near and far targets pick different encodings */
        { void* near_t = (void*)((char*)g_buf + 0x100);
          void* far_t  = (void*)0x00007f0000000000ull;
          check("FastCall near",[&]{ xFastCall(near_t); },[&]{ shim_FastCall(near_t); });
          check("FastCall far", [&]{ xFastCall(far_t);  },[&]{ shim_FastCall(far_t);  }); }
    }

    printf("shim cases: %ld   divergent: %ld\n",g_cases,g_fail);
    for(size_t i=0;i<g_f.size();i++) printf("  %s\n",g_f[i].c_str());
    return g_fail?1:0;
}
