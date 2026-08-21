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

    /* xBSR and the high/low SSE moves. BSR takes its 0x66 prefix from the
       source in the register form and from the destination in the memory
       form; MOVH/MOVL use Opcode for loads and Opcode+1 for stores. */
    { const shim_BitScan sBSR={0xbd};
      const shim_MovHL sMH={0x16}, sML={0x12};
      const shim_MovHL_RtoR sLH={0x16}, sHL={0x12};
      for(int a=0;a<16;a++){
        for(int b=0;b<16;b++){
          snprintf(nm,sizeof nm,"BSR32 %d,%d",a,b);
          ck(nm,[&]{xBSR(xRegister32(a),xRegister32(b));},[&]{sBSR(xRegister32(a),xRegister32(b));});
          snprintf(nm,sizeof nm,"BSR64 %d,%d",a,b);
          ck(nm,[&]{xBSR(xRegister64(a),xRegister64(b));},[&]{sBSR(xRegister64(a),xRegister64(b));});
          snprintf(nm,sizeof nm,"BSR16 %d,%d",a,b);
          ck(nm,[&]{xBSR(xRegister16(a),xRegister16(b));},[&]{sBSR(xRegister16(a),xRegister16(b));});
          snprintf(nm,sizeof nm,"MOVLH %d,%d",a,b);
          ck(nm,[&]{xMOVLH.PS(xRegisterSSE(a),xRegisterSSE(b));},[&]{sLH.PS(xRegisterSSE(a),xRegisterSSE(b));});
          snprintf(nm,sizeof nm,"MOVHL %d,%d",a,b);
          ck(nm,[&]{xMOVHL.PS(xRegisterSSE(a),xRegisterSSE(b));},[&]{sHL.PS(xRegisterSSE(a),xRegisterSSE(b));});
          snprintf(nm,sizeof nm,"MOVLH.PD %d,%d",a,b);
          ck(nm,[&]{xMOVLH.PD(xRegisterSSE(a),xRegisterSSE(b));},[&]{sLH.PD(xRegisterSSE(a),xRegisterSSE(b));}); }
        for(int bs=0;bs<16;bs++){
          auto M=[&]{return ptr64[xAddressVoid(xAddressReg(bs),xAddressReg(2),4,0x20)];};
          auto M32=[&]{return ptr32[xAddressVoid(xAddressReg(bs),xAddressReg(2),4,0x20)];};
          snprintf(nm,sizeof nm,"BSR32_m %d b%d",a,bs);
          ck(nm,[&]{xBSR(xRegister32(a),M32());},[&]{sBSR(xRegister32(a),M32());});
          snprintf(nm,sizeof nm,"BSR64_m %d b%d",a,bs);
          ck(nm,[&]{xBSR(xRegister64(a),M());},[&]{sBSR(xRegister64(a),M());});
          snprintf(nm,sizeof nm,"MOVH.PS_ld %d b%d",a,bs);
          ck(nm,[&]{xMOVH.PS(xRegisterSSE(a),M());},[&]{sMH.PS(xRegisterSSE(a),M());});
          snprintf(nm,sizeof nm,"MOVH.PS_st %d b%d",a,bs);
          ck(nm,[&]{xMOVH.PS(M(),xRegisterSSE(a));},[&]{sMH.PS(M(),xRegisterSSE(a));});
          snprintf(nm,sizeof nm,"MOVL.PD_ld %d b%d",a,bs);
          ck(nm,[&]{xMOVL.PD(xRegisterSSE(a),M());},[&]{sML.PD(xRegisterSSE(a),M());});
          snprintf(nm,sizeof nm,"MOVL.PD_st %d b%d",a,bs);
          ck(nm,[&]{xMOVL.PD(M(),xRegisterSSE(a));},[&]{sML.PD(M(),xRegisterSSE(a));}); } } }


    /* PINSR / PEXTR. Note PExtract::W uses 0xc5 for a register destination
       but 0x153a for a memory one -- different opcodes -- and the Q members
       take 64-bit operands, so REX.W applies. */
    { const shim_PInsert sPI; const shim_PExtract sPE;
      const u8 iv[5]={0,1,3,7,0xff};
      for(int x=0;x<16;x++) for(int g=0;g<16;g++) for(int i=0;i<5;i++){
        snprintf(nm,sizeof nm,"PINSR.B x%d r%d i%d",x,g,i);
        ck(nm,[&]{xPINSR.B(xRegisterSSE(x),xRegister32(g),iv[i]);},[&]{sPI.B(xRegisterSSE(x),xRegister32(g),iv[i]);});
        snprintf(nm,sizeof nm,"PINSR.W x%d r%d i%d",x,g,i);
        ck(nm,[&]{xPINSR.W(xRegisterSSE(x),xRegister32(g),iv[i]);},[&]{sPI.W(xRegisterSSE(x),xRegister32(g),iv[i]);});
        snprintf(nm,sizeof nm,"PINSR.D x%d r%d i%d",x,g,i);
        ck(nm,[&]{xPINSR.D(xRegisterSSE(x),xRegister32(g),iv[i]);},[&]{sPI.D(xRegisterSSE(x),xRegister32(g),iv[i]);});
        snprintf(nm,sizeof nm,"PINSR.Q x%d r%d i%d",x,g,i);
        ck(nm,[&]{xPINSR.Q(xRegisterSSE(x),xRegister64(g),iv[i]);},[&]{sPI.Q(xRegisterSSE(x),xRegister64(g),iv[i]);});
        snprintf(nm,sizeof nm,"PEXTR.B r%d x%d i%d",g,x,i);
        ck(nm,[&]{xPEXTR.B(xRegister32(g),xRegisterSSE(x),iv[i]);},[&]{sPE.B(xRegister32(g),xRegisterSSE(x),iv[i]);});
        snprintf(nm,sizeof nm,"PEXTR.W r%d x%d i%d",g,x,i);
        ck(nm,[&]{xPEXTR.W(xRegister32(g),xRegisterSSE(x),iv[i]);},[&]{sPE.W(xRegister32(g),xRegisterSSE(x),iv[i]);});
        snprintf(nm,sizeof nm,"PEXTR.D r%d x%d i%d",g,x,i);
        ck(nm,[&]{xPEXTR.D(xRegister32(g),xRegisterSSE(x),iv[i]);},[&]{sPE.D(xRegister32(g),xRegisterSSE(x),iv[i]);});
        snprintf(nm,sizeof nm,"PEXTR.Q r%d x%d i%d",g,x,i);
        ck(nm,[&]{xPEXTR.Q(xRegister64(g),xRegisterSSE(x),iv[i]);},[&]{sPE.Q(xRegister64(g),xRegisterSSE(x),iv[i]);}); }
      for(int x=0;x<16;x++) for(int b=0;b<16;b++){
        auto M32=[&]{return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x14)];};
        auto M64=[&]{return ptr64[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x14)];};
        snprintf(nm,sizeof nm,"PINSR.B_m x%d b%d",x,b);
        ck(nm,[&]{xPINSR.B(xRegisterSSE(x),M32(),3);},[&]{sPI.B(xRegisterSSE(x),M32(),3);});
        snprintf(nm,sizeof nm,"PINSR.Q_m x%d b%d",x,b);
        ck(nm,[&]{xPINSR.Q(xRegisterSSE(x),M64(),3);},[&]{sPI.Q(xRegisterSSE(x),M64(),3);});
        snprintf(nm,sizeof nm,"PEXTR.W_m x%d b%d",x,b);
        ck(nm,[&]{xPEXTR.W(M32(),xRegisterSSE(x),3);},[&]{sPE.W(M32(),xRegisterSSE(x),3);});
        snprintf(nm,sizeof nm,"PEXTR.D_m x%d b%d",x,b);
        ck(nm,[&]{xPEXTR.D(M32(),xRegisterSSE(x),3);},[&]{sPE.D(M32(),xRegisterSSE(x),3);});
        snprintf(nm,sizeof nm,"PEXTR.Q_m x%d b%d",x,b);
        ck(nm,[&]{xPEXTR.Q(M64(),xRegisterSSE(x),3);},[&]{sPE.Q(M64(),xRegisterSSE(x),3);}); } }


    /* xLEA at all three widths. EmitLeaMagic rewrites several shapes into
       shorter instructions -- a displacement-only source becomes a MOV, a
       bare base becomes a MOV, a base+index with no displacement stays an
       LEA, and preserve_flags suppresses the rewrites that would clobber
       them -- so the matrix drives each shape with the flag both ways. */
    { const s32 ld[6]={0,1,0x7f,0x80,-1,-0x1000};
      for(int d=0;d<16;d++) for(int pf=0;pf<2;pf++){
        for(int i=0;i<6;i++){
          /* displacement only */
          snprintf(nm,sizeof nm,"LEA64_d %d p%d i%d",d,pf,i);
          ck(nm,[&]{xLEA(xRegister64(d),ptr[(void*)(uptr)(u32)ld[i]],pf);},
               [&]{shim_LEA64(xRegister64(d),ptr[(void*)(uptr)(u32)ld[i]],pf);});
          /* base only */
          snprintf(nm,sizeof nm,"LEA32_b %d p%d i%d",d,pf,i);
          ck(nm,[&]{xLEA(xRegister32(d),ptr[xAddressVoid(xAddressReg(i%8),ld[i])],pf);},
               [&]{shim_LEA32(xRegister32(d),ptr[xAddressVoid(xAddressReg(i%8),ld[i])],pf);});
          /* no LEA16 here: the 16-bit form is deliberately not switched --
             its peephole rewrites prefix each inner operation as well as the
             outer write, which E_LEA_SZ cannot express from a single width
             flag, and nothing in the recompilers calls it. */ }
        for(int b=0;b<16;b++) for(int ix=0;ix<16;ix++){ if(ix==4) continue;
          for(int sc=0;sc<4;sc++){
            const int SS[4]={1,2,4,8};
            auto A=[&]{return xAddressVoid(xAddressReg(b),xAddressReg(ix),SS[sc],ld[(b+ix)%6]);};
            snprintf(nm,sizeof nm,"LEA64 %d p%d b%d i%d s%d",d,pf,b,ix,SS[sc]);
            ck(nm,[&]{xLEA(xRegister64(d),ptr[A()],pf);},[&]{shim_LEA64(xRegister64(d),ptr[A()],pf);});
            snprintf(nm,sizeof nm,"LEA32 %d p%d b%d i%d s%d",d,pf,b,ix,SS[sc]);
            ck(nm,[&]{xLEA(xRegister32(d),ptr[A()],pf);},[&]{shim_LEA32(xRegister32(d),ptr[A()],pf);}); } } } }


    /* AVX. Driven at xmm *and* ymm width, because the L bit is zero for xmm
       and an error in the width rule is invisible there. Also driven with
       extended base and index registers, which the two-byte VEX form cannot
       encode and which must therefore select the three-byte form. */
    { const shim_AVXMove sVAPS={0x00,0x28,0x29}, sVUPS={0x00,0x10,0x11};
      const shim_AVXThreeArg sVPAND={0x66,0xDB};
      const shim_AVXCmpInt sVPCMP={{0x66,0x74},{0x66,0x75},{0x66,0x76},
                                   {0x66,0x64},{0x66,0x65},{0x66,0x66}};
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"VMOVAPS x%d,x%d",a,b);
        ck(nm,[&]{xVMOVAPS(xRegisterSSE(a),xRegisterSSE(b));},[&]{sVAPS(xRegisterSSE(a),xRegisterSSE(b));});
        snprintf(nm,sizeof nm,"VMOVAPS y%d,y%d",a,b);
        ck(nm,[&]{xVMOVAPS(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()));},
             [&]{sVAPS(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()));});
        snprintf(nm,sizeof nm,"VPAND y%d,y%d",a,b);
        ck(nm,[&]{xVPAND(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()),xRegisterSSE((a+1)&15, xRegisterYMMTag()));},
             [&]{sVPAND(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()),xRegisterSSE((a+1)&15, xRegisterYMMTag()));});
        snprintf(nm,sizeof nm,"VPAND x%d,x%d",a,b);
        ck(nm,[&]{xVPAND(xRegisterSSE(a),xRegisterSSE(b),xRegisterSSE((a+1)&15));},
             [&]{sVPAND(xRegisterSSE(a),xRegisterSSE(b),xRegisterSSE((a+1)&15));});
        snprintf(nm,sizeof nm,"VPCMP.EQD y%d,y%d",a,b);
        ck(nm,[&]{xVPCMP.EQD(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()),xRegisterSSE((b+2)&15, xRegisterYMMTag()));},
             [&]{sVPCMP.EQD(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(b, xRegisterYMMTag()),xRegisterSSE((b+2)&15, xRegisterYMMTag()));});
        snprintf(nm,sizeof nm,"VPCMP.GTB x%d,x%d",a,b);
        ck(nm,[&]{xVPCMP.GTB(xRegisterSSE(a),xRegisterSSE(b),xRegisterSSE((b+2)&15));},
             [&]{sVPCMP.GTB(xRegisterSSE(a),xRegisterSSE(b),xRegisterSSE((b+2)&15));}); }
      for(int a=0;a<16;a++) for(int bs=0;bs<16;bs++) for(int ix=0;ix<16;ix++){
        if(ix==4) continue;
        auto M=[&]{return ptr[xAddressVoid(xAddressReg(bs),xAddressReg(ix),4,0x20)];};
        snprintf(nm,sizeof nm,"VMOVAPS_ld y%d b%d i%d",a,bs,ix);
        ck(nm,[&]{xVMOVAPS(xRegisterSSE(a, xRegisterYMMTag()),M());},[&]{sVAPS(xRegisterSSE(a, xRegisterYMMTag()),M());});
        snprintf(nm,sizeof nm,"VMOVUPS_st y%d b%d i%d",a,bs,ix);
        ck(nm,[&]{xVMOVUPS(M(),xRegisterSSE(a, xRegisterYMMTag()));},[&]{sVUPS(M(),xRegisterSSE(a, xRegisterYMMTag()));});
        snprintf(nm,sizeof nm,"VMOVAPS_ld x%d b%d i%d",a,bs,ix);
        ck(nm,[&]{xVMOVAPS(xRegisterSSE(a),M());},[&]{sVAPS(xRegisterSSE(a),M());});
        snprintf(nm,sizeof nm,"VPCMP.EQD_m y%d b%d i%d",a,bs,ix);
        ck(nm,[&]{xVPCMP.EQD(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(1, xRegisterYMMTag()),M());},
             [&]{sVPCMP.EQD(xRegisterSSE(a, xRegisterYMMTag()),xRegisterSSE(1, xRegisterYMMTag()),M());}); }
      /* base-only, which routes through the no-SIB branch where X folds into B */
      for(int a=0;a<16;a++) for(int bs=0;bs<16;bs++){
        auto Mb=[&]{return ptr[xAddressVoid(xAddressReg(bs),0x20)];};
        snprintf(nm,sizeof nm,"VMOVAPS_b y%d b%d",a,bs);
        ck(nm,[&]{xVMOVAPS(xRegisterSSE(a, xRegisterYMMTag()),Mb());},[&]{sVAPS(xRegisterSSE(a, xRegisterYMMTag()),Mb());}); } }


    /* CMOVcc and SETcc. CMov at all three widths against registers and
       memory; SETcc across all twenty 8-bit destination ids -- the 0x10
       marker registers being the recurring trap -- and against memory. */
    { const shim_CMov cB={Jcc_Below},cGE={Jcc_GreaterOrEqual},cE={Jcc_Equal},
                      cNE={Jcc_NotEqual},cS={Jcc_Signed},cNS={Jcc_Unsigned};
      const shim_Set sA={Jcc_Above},sB={Jcc_Below},sG={Jcc_Greater},sL={Jcc_Less};
      static const int q8b[20]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0x14,0x15,0x16,0x17};
      struct { const char* n; const void* r; const shim_CMov* s; } cm[6] = {
        {"CMOVB",&xCMOVB,&cB},{"CMOVGE",&xCMOVGE,&cGE},{"CMOVE",&xCMOVE,&cE},
        {"CMOVNE",&xCMOVNE,&cNE},{"CMOVS",&xCMOVS,&cS},{"CMOVNS",&xCMOVNS,&cNS}};
      (void)cm;
      for(int d=0;d<16;d++) for(int f2=0;f2<16;f2++){
        snprintf(nm,sizeof nm,"CMOVB32 %d,%d",d,f2);
        ck(nm,[&]{xCMOVB(xRegister32(d),xRegister32(f2));},[&]{cB(xRegister32(d),xRegister32(f2));});
        snprintf(nm,sizeof nm,"CMOVS64 %d,%d",d,f2);
        ck(nm,[&]{xCMOVS(xRegister64(d),xRegister64(f2));},[&]{cS(xRegister64(d),xRegister64(f2));});
        snprintf(nm,sizeof nm,"CMOVNE16 %d,%d",d,f2);
        ck(nm,[&]{xCMOVNE(xRegister16(d),xRegister16(f2));},[&]{cNE(xRegister16(d),xRegister16(f2));});
        snprintf(nm,sizeof nm,"CMOVGE32 %d,%d",d,f2);
        ck(nm,[&]{xCMOVGE(xRegister32(d),xRegister32(f2));},[&]{cGE(xRegister32(d),xRegister32(f2));});
        snprintf(nm,sizeof nm,"CMOVE64 %d,%d",d,f2);
        ck(nm,[&]{xCMOVE(xRegister64(d),xRegister64(f2));},[&]{cE(xRegister64(d),xRegister64(f2));});
        snprintf(nm,sizeof nm,"CMOVNS32 %d,%d",d,f2);
        ck(nm,[&]{xCMOVNS(xRegister32(d),xRegister32(f2));},[&]{cNS(xRegister32(d),xRegister32(f2));}); }
      for(int d=0;d<16;d++) for(int b=0;b<16;b++){
        auto M=[&]{return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        auto M64=[&]{return ptr64[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        snprintf(nm,sizeof nm,"CMOVB32_m %d b%d",d,b);
        ck(nm,[&]{xCMOVB(xRegister32(d),M());},[&]{cB(xRegister32(d),M());});
        snprintf(nm,sizeof nm,"CMOVS64_m %d b%d",d,b);
        ck(nm,[&]{xCMOVS(xRegister64(d),M64());},[&]{cS(xRegister64(d),M64());}); }
      for(int k=0;k<20;k++){
        snprintf(nm,sizeof nm,"SETA r%x",q8b[k]);
        ck(nm,[&]{xSETA(xRegister8(q8b[k]));},[&]{sA(xRegister8(q8b[k]));});
        snprintf(nm,sizeof nm,"SETB r%x",q8b[k]);
        ck(nm,[&]{xSETB(xRegister8(q8b[k]));},[&]{sB(xRegister8(q8b[k]));});
        snprintf(nm,sizeof nm,"SETG r%x",q8b[k]);
        ck(nm,[&]{xSETG(xRegister8(q8b[k]));},[&]{sG(xRegister8(q8b[k]));});
        snprintf(nm,sizeof nm,"SETL r%x",q8b[k]);
        ck(nm,[&]{xSETL(xRegister8(q8b[k]));},[&]{sL(xRegister8(q8b[k]));}); }
      for(int b=0;b<16;b++){
        auto M8=[&]{return ptr8[xAddressVoid(xAddressReg(b),xAddressReg(2),2,0x11)];};
        snprintf(nm,sizeof nm,"SETA_m b%d",b);
        ck(nm,[&]{xSETA(M8());},[&]{sA(M8());});
        snprintf(nm,sizeof nm,"SETL_m b%d",b);
        ck(nm,[&]{xSETL(M8());},[&]{sL(M8());}); } }


    /* Free-function encoders. xMOVD's GPR may be 64-bit and REX.W follows
       it; xMOVSS/xMOVSD reg-reg elide self-moves; PUSH/POP are implicitly
       wide so REX.W is never emitted, only REX.B for r8-r15; xPUSH imm
       narrows to the s8 form when it fits. */
    { for(int a=0;a<16;a++) for(int g=0;g<16;g++){
        snprintf(nm,sizeof nm,"MOVDZX x%d,r%d",a,g);
        ck(nm,[&]{xMOVDZX(xRegisterSSE(a),xRegister32(g));},[&]{shim_xMOVDZX(xRegisterSSE(a),xRegister32(g));});
        snprintf(nm,sizeof nm,"MOVDZX64 x%d,r%d",a,g);
        ck(nm,[&]{xMOVDZX(xRegisterSSE(a),xRegister64(g));},[&]{shim_xMOVDZX(xRegisterSSE(a),xRegister64(g));});
        snprintf(nm,sizeof nm,"MOVD r%d,x%d",g,a);
        ck(nm,[&]{xMOVD(xRegister32(g),xRegisterSSE(a));},[&]{shim_xMOVD(xRegister32(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"MOVD64 r%d,x%d",g,a);
        ck(nm,[&]{xMOVD(xRegister64(g),xRegisterSSE(a));},[&]{shim_xMOVD(xRegister64(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"MOVQZX x%d,x%d",a,g);
        ck(nm,[&]{xMOVQZX(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xMOVQZX(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"MOVSS x%d,x%d",a,g);
        ck(nm,[&]{xMOVSS(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xMOVSS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"MOVSD x%d,x%d",a,g);
        ck(nm,[&]{xMOVSD(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xMOVSD(xRegisterSSE(a),xRegisterSSE(g));}); }
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        auto M=[&]{return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        auto MV=[&]{return ptr[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        snprintf(nm,sizeof nm,"MOVDZX_m x%d b%d",a,b);
        ck(nm,[&]{xMOVDZX(xRegisterSSE(a),MV());},[&]{shim_xMOVDZX(xRegisterSSE(a),MV());});
        snprintf(nm,sizeof nm,"MOVD_m x%d b%d",a,b);
        ck(nm,[&]{xMOVD(MV(),xRegisterSSE(a));},[&]{shim_xMOVD(MV(),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"MOVSSZX_m x%d b%d",a,b);
        ck(nm,[&]{xMOVSSZX(xRegisterSSE(a),MV());},[&]{shim_xMOVSSZX(xRegisterSSE(a),MV());});
        snprintf(nm,sizeof nm,"MOVSS_st x%d b%d",a,b);
        ck(nm,[&]{xMOVSS(MV(),xRegisterSSE(a));},[&]{shim_xMOVSS(MV(),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"MOVSDZX_m x%d b%d",a,b);
        ck(nm,[&]{xMOVSDZX(xRegisterSSE(a),MV());},[&]{shim_xMOVSDZX(xRegisterSSE(a),MV());});
        snprintf(nm,sizeof nm,"MOVQZX_m x%d b%d",a,b);
        ck(nm,[&]{xMOVQZX(xRegisterSSE(a),MV());},[&]{shim_xMOVQZX(xRegisterSSE(a),MV());});
        snprintf(nm,sizeof nm,"MOVQ_st x%d b%d",a,b);
        ck(nm,[&]{xMOVQ(MV(),xRegisterSSE(a));},[&]{shim_xMOVQ(MV(),xRegisterSSE(a));});
        (void)M; }
      static const u32 pim[6]={0,1,0x7f,0x80,0x1234,0xffffffffu};
      for(int r=0;r<16;r++){
        snprintf(nm,sizeof nm,"PUSH r%d",r);
        ck(nm,[&]{xPUSH(xRegister64(r));},[&]{shim_xPUSH(xRegister64(r));});
        snprintf(nm,sizeof nm,"POP r%d",r);
        ck(nm,[&]{xPOP(xRegister64(r));},[&]{shim_xPOP(xRegister64(r));}); }
      for(int i=0;i<6;i++){
        snprintf(nm,sizeof nm,"PUSH imm%d",i);
        ck(nm,[&]{xPUSH(pim[i]);},[&]{shim_xPUSH(pim[i]);}); }
      for(int b=0;b<16;b++){
        auto MV=[&]{return ptrNative[xAddressVoid(xAddressReg(b),xAddressReg(2),2,0x40)];};
        snprintf(nm,sizeof nm,"PUSH_m b%d",b);
        ck(nm,[&]{xPUSH(MV());},[&]{shim_xPUSH(MV());});
        snprintf(nm,sizeof nm,"POP_m b%d",b);
        ck(nm,[&]{xPOP(MV());},[&]{shim_xPOP(MV());}); } }


    /* CVT family, the CMP compare objects, INSERTPS and EXTRACTPS's memory
       form. The GPR in the CVT scalar forms carries REX.W from its own
       width -- source for SI2SS, destination for the *2SI group. */
    { for(int a=0;a<16;a++) for(int g=0;g<16;g++){
        snprintf(nm,sizeof nm,"CVTSI2SS x%d,r%d",a,g);
        ck(nm,[&]{xCVTSI2SS(xRegisterSSE(a),xRegister32(g));},[&]{shim_xCVTSI2SS(xRegisterSSE(a),xRegister32(g));});
        snprintf(nm,sizeof nm,"CVTSI2SS64 x%d,r%d",a,g);
        ck(nm,[&]{xCVTSI2SS(xRegisterSSE(a),xRegister64(g));},[&]{shim_xCVTSI2SS(xRegisterSSE(a),xRegister64(g));});
        snprintf(nm,sizeof nm,"CVTTSS2SI r%d,x%d",g,a);
        ck(nm,[&]{xCVTTSS2SI(xRegister32(g),xRegisterSSE(a));},[&]{shim_xCVTTSS2SI(xRegister32(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"CVTTSS2SI64 r%d,x%d",g,a);
        ck(nm,[&]{xCVTTSS2SI(xRegister64(g),xRegisterSSE(a));},[&]{shim_xCVTTSS2SI(xRegister64(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"CVTTSD2SI r%d,x%d",g,a);
        ck(nm,[&]{xCVTTSD2SI(xRegister32(g),xRegisterSSE(a));},[&]{shim_xCVTTSD2SI(xRegister32(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"CVTSS2SI r%d,x%d",g,a);
        ck(nm,[&]{xCVTSS2SI(xRegister32(g),xRegisterSSE(a));},[&]{shim_xCVTSS2SI(xRegister32(g),xRegisterSSE(a));});
        snprintf(nm,sizeof nm,"CVTDQ2PS x%d,x%d",a,g);
        ck(nm,[&]{xCVTDQ2PS(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xCVTDQ2PS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CVTSD2SS x%d,x%d",a,g);
        ck(nm,[&]{xCVTSD2SS(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xCVTSD2SS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CVTSS2SD x%d,x%d",a,g);
        ck(nm,[&]{xCVTSS2SD(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xCVTSS2SD(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CVTTPS2DQ x%d,x%d",a,g);
        ck(nm,[&]{xCVTTPS2DQ(xRegisterSSE(a),xRegisterSSE(g));},[&]{shim_xCVTTPS2DQ(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CMPEQ.SS x%d,x%d",a,g);
        ck(nm,[&]{xCMPEQ.SS(xRegisterSSE(a),xRegisterSSE(g));},
             [&]{shim_SimdCompare{SSE2_Equal}.SS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CMPEQ.PS x%d,x%d",a,g);
        ck(nm,[&]{xCMPEQ.PS(xRegisterSSE(a),xRegisterSSE(g));},
             [&]{shim_SimdCompare{SSE2_Equal}.PS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CMPNLT.PS x%d,x%d",a,g);
        ck(nm,[&]{xCMPNLT.PS(xRegisterSSE(a),xRegisterSSE(g));},
             [&]{shim_SimdCompare{SSE2_NotLess}.PS(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"CMPNLE.SD x%d,x%d",a,g);
        ck(nm,[&]{xCMPNLE.SD(xRegisterSSE(a),xRegisterSSE(g));},
             [&]{shim_SimdCompare{SSE2_NotLessOrEqual}.SD(xRegisterSSE(a),xRegisterSSE(g));});
        snprintf(nm,sizeof nm,"INSERTPS x%d,x%d",a,g);
        ck(nm,[&]{xINSERTPS(xRegisterSSE(a),xRegisterSSE(g),0x5c);},
             [&]{shim_xINSERTPS(xRegisterSSE(a),xRegisterSSE(g),0x5c);}); }
      for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        auto M32=[&]{return ptr32[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        auto M64=[&]{return ptr64[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        auto M128=[&]{return ptr128[xAddressVoid(xAddressReg(b),xAddressReg(1),4,0x18)];};
        snprintf(nm,sizeof nm,"CVTSI2SS_m x%d b%d",a,b);
        ck(nm,[&]{xCVTSI2SS(xRegisterSSE(a),M32());},[&]{shim_xCVTSI2SS(xRegisterSSE(a),M32());});
        snprintf(nm,sizeof nm,"CVTTSD2SI_m r%d b%d",a,b);
        ck(nm,[&]{xCVTTSD2SI(xRegister64(a),M64());},[&]{shim_xCVTTSD2SI(xRegister64(a),M64());});
        snprintf(nm,sizeof nm,"CVTDQ2PS_m x%d b%d",a,b);
        ck(nm,[&]{xCVTDQ2PS(xRegisterSSE(a),M128());},[&]{shim_xCVTDQ2PS(xRegisterSSE(a),M128());});
        snprintf(nm,sizeof nm,"CMPEQ.SS_m x%d b%d",a,b);
        ck(nm,[&]{xCMPEQ.SS(xRegisterSSE(a),M32());},
             [&]{shim_SimdCompare{SSE2_Equal}.SS(xRegisterSSE(a),M32());});
        snprintf(nm,sizeof nm,"EXTRACTPS_m x%d b%d",a,b);
        ck(nm,[&]{xEXTRACTPS(M32(),xRegisterSSE(a),2);},[&]{shim_xEXTRACTPS(M32(),xRegisterSSE(a),2);}); } }


    /* xVPMOVMSKB at both widths: c5 f9 d7 vs c5 fd d7, one L bit apart. */
    { for(int g=0;g<16;g++) for(int x=0;x<16;x++){
        snprintf(nm,sizeof nm,"VPMOVMSKB x %d,%d",g,x);
        ck(nm,[&]{ xVPMOVMSKB(xRegister32(g), xRegisterSSE(x)); },
             [&]{ e_u8* q=(e_u8*)x86Ptr;
                  E_VEX_RRR(q,0x66,0xd7,xRegister32(g).Id,E_NOREG,xRegisterSSE(x).Id,0);
                  x86Ptr=(u8*)q; });
        snprintf(nm,sizeof nm,"VPMOVMSKB y %d,%d",g,x);
        ck(nm,[&]{ xVPMOVMSKB(xRegister32(g), xRegisterSSE(x, xRegisterYMMTag())); },
             [&]{ e_u8* q=(e_u8*)x86Ptr;
                  E_VEX_RRR(q,0x66,0xd7,xRegister32(g).Id,E_NOREG,xRegisterSSE(x, xRegisterYMMTag()).Id,1);
                  x86Ptr=(u8*)q; }); } }


    /* xPMOVMSKB, plain SSE form, both GPR widths. */
    { for(int g=0;g<16;g++) for(int x=0;x<16;x++){
        snprintf(nm,sizeof nm,"PMOVMSKB %d,%d",g,x);
        ck(nm,[&]{xPMOVMSKB(xRegister32(g),xRegisterSSE(x));},[&]{shim_xPMOVMSKB(xRegister32(g),xRegisterSSE(x));});
        snprintf(nm,sizeof nm,"PMOVMSKB64 %d,%d",g,x);
        ck(nm,[&]{xPMOVMSKB(xRegister64(g),xRegisterSSE(x));},[&]{shim_xPMOVMSKB(xRegister64(g),xRegisterSSE(x));}); } }


    /* The scalar SSE/SSE2 ops the FPU recompilers use, macro against core.
       These were the legacy_instructions.h C wrappers until that header was
       retired; the macros are what the recompilers call now, so they are
       what gets oracled. */
    { struct W { const char* nm; int which; e_u8 pre, op; };
      static const W ws[] = {
        {"MAXSS", 0, 0xf3, 0x5f}, {"MINSS", 1, 0xf3, 0x5d},
        {"ADDSS", 2, 0xf3, 0x58}, {"SUBSS", 3, 0xf3, 0x5c},
        {"MAXSD", 4, 0xf2, 0x5f}, {"MINSD", 5, 0xf2, 0x5d},
        {"ADDSD", 6, 0xf2, 0x58}, {"SUBSD", 7, 0xf2, 0x5c},
      };
      for (const W& w : ws) for(int a=0;a<16;a++) for(int b=0;b<16;b++){
        snprintf(nm,sizeof nm,"%s %d,%d",w.nm,a,b);
        ck(nm,[&]{ switch (w.which) {
                     case 0: xe_maxss_xx(a,b); break; case 1: xe_minss_xx(a,b); break;
                     case 2: xe_addss_xx(a,b); break; case 3: xe_subss_xx(a,b); break;
                     case 4: xe_maxsd_xx(a,b); break; case 5: xe_minsd_xx(a,b); break;
                     case 6: xe_addsd_xx(a,b); break; default: xe_subsd_xx(a,b); break; } },
             [&]{ e_u8* q=(e_u8*)x86Ptr; E_SSE_RR(q, w.pre, w.op, a, b); x86Ptr=(u8*)q; }); } }

    printf("newly bound: cases %ld | divergent %ld\n",C,F);
    return F?1:0; }
