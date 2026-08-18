/* Does the inline fast path select exactly the addresses the original
 * function handled with its inline return, and produce the same value?
 * Both predicates are pure arithmetic over the address plus a LUT lookup,
 * so this runs without the emulator. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32; typedef uint8_t u8; typedef uintptr_t uptr;

static uptr LUT[0x2000];
static u8   PAGE[0x10000];

/* Each width has its own excluded pages, transcribed from its own original.
 * read8 skips 0x1f80 and 0x1f40 and has no SBUS check; read16 and read32 skip
 * 0x1f80 and, after the lookup, 0x1d00. Confusing the two would give a fast
 * path that bypassed a hardware register. */
static int ref8_takes_inline(u32 mem, u32* out){
    mem &= 0x1fffffff;
    u32 t = mem >> 16;
    if (t == 0x1f80) return 0;
    if (t == 0x1f40) return 0;
    const u8* p = (const u8*)(LUT[mem >> 16]);
    if (p == NULL) return 0;
    *out = *(const u8*)(p + (mem & 0xffff));
    return 1;
}
static int new8_takes_fast(u32 mem, u32* out){
    mem &= 0x1fffffff;
    { const u32 t = mem >> 16;
      if (t != 0x1f80 && t != 0x1f40){
        const u8* const p = (const u8*)(LUT[t]);
        if (p != NULL){ *out = *(const u8*)(p + (mem & 0xffff)); return 1; } } }
    return 0;
}
static int ref16_takes_inline(u32 mem, u32* out){
    mem &= 0x1fffffff;
    u32 t = mem >> 16;
    if (t == 0x1f80) return 0;
    const u8* p = (const u8*)(LUT[mem >> 16]);
    if (p == NULL) return 0;
    if (t == 0x1d00) return 0;
    *out = *(const uint16_t*)(p + (mem & 0xffff));
    return 1;
}
static int new16_takes_fast(u32 mem, u32* out){
    mem &= 0x1fffffff;
    { const u32 t = mem >> 16;
      if (t != 0x1f80 && t != 0x1d00){
        const u8* const p = (const u8*)(LUT[t]);
        if (p != NULL){ *out = *(const uint16_t*)(p + (mem & 0xffff)); return 1; } } }
    return 0;
}

/* transcribed from the original iopMemRead32 control flow */
static int ref_takes_inline_return(u32 mem, u32* out){
    mem &= 0x1fffffff;
    u32 t = mem >> 16;
    if (t == 0x1f80) return 0;                 /* hardware page */
    const u8* p = (const u8*)(LUT[mem >> 16]);
    if (p == NULL) return 0;                   /* unmapped */
    if (t == 0x1d00) return 0;                 /* SBUS window */
    *out = *(const u32*)(p + (mem & 0xffff));
    return 1;
}
/* the new inline, copied verbatim from IopMem.h */
static int new_takes_fast(u32 mem, u32* out){
    mem &= 0x1fffffff;
    {
        const u32 t = mem >> 16;
        if (t != 0x1f80 && t != 0x1d00){
            const u8* const p = (const u8*)(LUT[t]);
            if (p != NULL){ *out = *(const u32*)(p + (mem & 0xffff)); return 1; }
        }
    }
    return 0;
}
int main(void){
    long cases=0, mismatch=0;
    unsigned i;
    for (i=0;i<0x10000;i++) PAGE[i]=(u8)(i*7+3);
    /* map a representative spread of pages, leave others NULL */
    for (i=0;i<0x2000;i++) LUT[i] = ((i % 3) == 0) ? (uptr)PAGE : 0;
    for (u32 hi=0; hi<0x2000; hi++){
        static const u32 los[6]={0,4,0x1000,0x8ffc,0xfffc,0x3f0};
        for (int k=0;k<6;k++){
            u32 mem = (hi<<16)|los[k];
            u32 a=0xDEAD,b=0xBEEF;
            int ta=ref_takes_inline_return(mem,&a);
            int tb=new_takes_fast(mem,&b);
            cases++;
            if (ta!=tb || (ta && a!=b)){
                if (mismatch<5) printf("  mem=%08x ref_taken=%d(%08x) new_taken=%d(%08x)\n",mem,ta,a,tb,b);
                mismatch++;
            }
            /* the 8- and 16-bit widths, each against its own original */
            { u32 c=0xDEAD,d=0xBEEF;
              int t8a=ref8_takes_inline(mem,&c), t8b=new8_takes_fast(mem,&d);
              cases++;
              if (t8a!=t8b || (t8a && c!=d)){
                if (mismatch<5) printf("  r8  mem=%08x ref=%d(%08x) new=%d(%08x)\n",mem,t8a,c,t8b,d);
                mismatch++; }
              c=0xDEAD; d=0xBEEF;
              int t16a=ref16_takes_inline(mem,&c), t16b=new16_takes_fast(mem,&d);
              cases++;
              if (t16a!=t16b || (t16a && c!=d)){
                if (mismatch<5) printf("  r16 mem=%08x ref=%d(%08x) new=%d(%08x)\n",mem,t16a,c,t16b,d);
                mismatch++; } }

            /* and again with the high bits set, which the mask must strip */
            u32 mem2 = mem | 0x80000000u;
            a=0xDEAD;b=0xBEEF;
            ta=ref_takes_inline_return(mem2,&a); tb=new_takes_fast(mem2,&b);
            cases++;
            if (ta!=tb || (ta && a!=b)){
                if (mismatch<5) printf("  mem=%08x ref=%d(%08x) new=%d(%08x)\n",mem2,ta,a,tb,b);
                mismatch++;
            }
        }
    }
    printf("iop read split (8/16/32): %ld cases | mismatches %ld\n",cases,mismatch);
    return mismatch?1:0;
}
