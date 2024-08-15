/*  Pcsx2 - Pc Ps2 Emulator
 *  Copyright (C) 2002-2008  Pcsx2 Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/*  This code was based on the FPSE v0.08 Mdec decoder*/

#include <stdio.h>
#include <string.h>
#include "Common.h"

#include "Mdec.h"
#include "IopHw.h"

struct {
	u32 command;
	u32 status;
	u16 *rl;
	int rlsize;
} mdec;

struct config_mdec {
	u32 Mdec;
};
struct config_mdec Config;

// Should be optimized(the funcs. that use it) to read IOP RAM direcly.
#define PSXM(x) ((uintptr_t)mdecMem + x)

#define FIX_1_082392200  (277)
#define FIX_1_414213562  (362)
#define FIX_1_847759065  (473)
#define FIX_2_613125930  (669)

#define CONST_BITS  8
#define PASS1_BITS  2
#define CONST_BITS14 14
#define	IFAST_SCALE_BITS 2

#define	MAKERGB15(r,g,b)	( (((r)>>3)<<10)|(((g)>>3)<<5)|((b)>>3) )
#define	ROUND(c)	roundtbl[((c)+128+256)]//&0x3ff]

#define RGB15(n, Y) \
	image[n] = MAKERGB15(ROUND(Y + R),ROUND(Y + G),ROUND(Y + B));

#define RGB15BW(n, Y) \
	image[n] = MAKERGB15(ROUND(Y),ROUND(Y),ROUND(Y));

#define RGB24(n, Y) \
	image[n+2] = ROUND(Y + R); \
	image[n+1] = ROUND(Y + G); \
	image[n+0] = ROUND(Y + B);

#define RGB24BW(n, Y) \
	image[n+2] = ROUND(Y); \
	image[n+1] = ROUND(Y); \
	image[n+0] = ROUND(Y);

#define DESCALE(x,n)  ((x)>>(n))
#define RANGE(n) (n)

#define MULTIPLY(var,const)  (DESCALE((var) * (const), CONST_BITS))

#define DCTSIZE 8
#define DCTSIZE2 64

#define RUNOF(a) ((a)>>10)
#define VALOF(a) (((int)(a)<<(32-10))>>(32-10))
#define NOP	0xfe00

#define	MULR(a)		((((int)0x0000059B) * (a)) >> 10)
#define	MULG(a)		((((int)0xFFFFFEA1) * (a)) >> 10)
#define	MULG2(a)	((((int)0xFFFFFD25) * (a)) >> 10)
#define	MULB(a)		((((int)0x00000716) * (a)) >> 10)

static int iq_y[DCTSIZE2],iq_uv[DCTSIZE2];

static int zscan[DCTSIZE2] = {
	0 ,1 ,8 ,16,9 ,2 ,3 ,10,
	17,24,32,25,18,11,4 ,5 ,
	12,19,26,33,40,48,41,34,
	27,20,13,6 ,7 ,14,21,28,
	35,42,49,56,57,50,43,36,
	29,22,15,23,30,37,44,51,
	58,59,52,45,38,31,39,46,
	53,60,61,54,47,55,62,63
};

static int aanscales[DCTSIZE2] = {
	  16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	  22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
	  21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
	  19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
	  16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
	  12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
	   8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
	   4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247
};

static u32 mdecArr2[0x100000] = { 0 };

static u32 mdecMem[0x100000]; //watherver large size. //Memory only used to get DMA data and not really for anything else.

static unsigned char roundtbl[256*3];

static void round_init(void)
{
	int i;
	for(i=0;i<256;i++)
	{
		roundtbl[i]     = 0;
		roundtbl[i+256] = i;
		roundtbl[i+512] = 255;
	}
}

static void iqtab_init(int *iqtab,unsigned char *iq_y)
{
	int i;

	for(i=0;i<DCTSIZE2;i++)
		iqtab[i] = iq_y[i] * aanscales[zscan[i]]>>(CONST_BITS14-IFAST_SCALE_BITS);
}


static void idct1(int *block)
{
	int i, val;

	val = RANGE(DESCALE(block[0], PASS1_BITS+3));

	for(i=0;i<DCTSIZE2;i++)
		block[i]=val;
}

static void idct(int *block,int k)
{
  int tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
  int z5, z10, z11, z12, z13;
  int *ptr;
  int i;

  if (!k) { idct1(block); return; }

  ptr = block;
  for (i = 0; i< DCTSIZE; i++,ptr++) {

    if ((ptr[DCTSIZE*1] | ptr[DCTSIZE*2] | ptr[DCTSIZE*3] |
	 ptr[DCTSIZE*4] | ptr[DCTSIZE*5] | ptr[DCTSIZE*6] |
	 ptr[DCTSIZE*7]) == 0) {
      ptr[DCTSIZE*0] =
      ptr[DCTSIZE*1] =
      ptr[DCTSIZE*2] =
      ptr[DCTSIZE*3] =
      ptr[DCTSIZE*4] =
      ptr[DCTSIZE*5] =
      ptr[DCTSIZE*6] =
      ptr[DCTSIZE*7] =
      	ptr[DCTSIZE*0];

      continue;
    }

    z10 = ptr[DCTSIZE*0] + ptr[DCTSIZE*4];
    z11 = ptr[DCTSIZE*0] - ptr[DCTSIZE*4];
    z13 = ptr[DCTSIZE*2] + ptr[DCTSIZE*6];
    z12 = MULTIPLY(ptr[DCTSIZE*2] - ptr[DCTSIZE*6], FIX_1_414213562) - z13;

    tmp0 = z10 + z13;
    tmp3 = z10 - z13;
    tmp1 = z11 + z12;
    tmp2 = z11 - z12;

    z13 = ptr[DCTSIZE*3] + ptr[DCTSIZE*5];
    z10 = ptr[DCTSIZE*3] - ptr[DCTSIZE*5];
    z11 = ptr[DCTSIZE*1] + ptr[DCTSIZE*7];
    z12 = ptr[DCTSIZE*1] - ptr[DCTSIZE*7];

    z5 = MULTIPLY(z12 - z10, FIX_1_847759065);
    tmp7 = z11 + z13;
    tmp6 = MULTIPLY(z10, FIX_2_613125930) + z5 - tmp7;
    tmp5 = MULTIPLY(z11 - z13, FIX_1_414213562) - tmp6;
    tmp4 = MULTIPLY(z12, FIX_1_082392200) - z5 + tmp5;

    ptr[DCTSIZE*0] = (tmp0 + tmp7);
    ptr[DCTSIZE*7] = (tmp0 - tmp7);
    ptr[DCTSIZE*1] = (tmp1 + tmp6);
    ptr[DCTSIZE*6] = (tmp1 - tmp6);
    ptr[DCTSIZE*2] = (tmp2 + tmp5);
    ptr[DCTSIZE*5] = (tmp2 - tmp5);
    ptr[DCTSIZE*4] = (tmp3 + tmp4);
    ptr[DCTSIZE*3] = (tmp3 - tmp4);

  }

  ptr = block;
  for (i = 0; i < DCTSIZE; i++ ,ptr+=DCTSIZE) {

    if ((ptr[1] | ptr[2] | ptr[3] | ptr[4] | ptr[5] | ptr[6] |
	 ptr[7]) == 0) {
      ptr[0] =
      ptr[1] =
      ptr[2] =
      ptr[3] =
      ptr[4] =
      ptr[5] =
      ptr[6] =
      ptr[7] =
      	RANGE(DESCALE(ptr[0], PASS1_BITS+3));

      continue;
    }

    z10 = ptr[0] + ptr[4];
    z11 = ptr[0] - ptr[4];
    z13 = ptr[2] + ptr[6];
    z12 = MULTIPLY(ptr[2] - ptr[6], FIX_1_414213562) - z13;

    tmp0 = z10 + z13;
    tmp3 = z10 - z13;
    tmp1 = z11 + z12;
    tmp2 = z11 - z12;

    z13 = ptr[3] + ptr[5];
    z10 = ptr[3] - ptr[5];
    z11 = ptr[1] + ptr[7];
    z12 = ptr[1] - ptr[7];

    z5 = MULTIPLY(z12 - z10, FIX_1_847759065);
    tmp7 = z11 + z13;
    tmp6 = MULTIPLY(z10, FIX_2_613125930) + z5 - tmp7;
    tmp5 = MULTIPLY(z11 - z13, FIX_1_414213562) - tmp6;
    tmp4 = MULTIPLY(z12, FIX_1_082392200) - z5 + tmp5;

    ptr[0] = RANGE(DESCALE(tmp0 + tmp7, PASS1_BITS+3));
    ptr[7] = RANGE(DESCALE(tmp0 - tmp7, PASS1_BITS+3));
    ptr[1] = RANGE(DESCALE(tmp1 + tmp6, PASS1_BITS+3));
    ptr[6] = RANGE(DESCALE(tmp1 - tmp6, PASS1_BITS+3));
    ptr[2] = RANGE(DESCALE(tmp2 + tmp5, PASS1_BITS+3));
    ptr[5] = RANGE(DESCALE(tmp2 - tmp5, PASS1_BITS+3));
    ptr[4] = RANGE(DESCALE(tmp3 + tmp4, PASS1_BITS+3));
    ptr[3] = RANGE(DESCALE(tmp3 - tmp4, PASS1_BITS+3));

  }
}

static void yuv2rgb15(int *blk, uint16_t *image)
{
	int x,y;
	int *Yblk = blk+DCTSIZE2*2;
	int Cb,Cr,R,G,B;
	int *Cbblk = blk;
	int *Crblk = blk+DCTSIZE2;

	if (!(Config.Mdec&0x1))
	for (y=0;y<16;y+=2,Crblk+=4,Cbblk+=4,Yblk+=8,image+=24) {
		if (y==8) Yblk+=DCTSIZE2;
		for (x=0;x<4;x++,image+=2,Crblk++,Cbblk++,Yblk+=2) {
			Cr = *Crblk;
			Cb = *Cbblk;
			R = MULR(Cr);
			G = MULG(Cb) + MULG2(Cr);
			B = MULB(Cb);

			RGB15(0, Yblk[0]);
			RGB15(1, Yblk[1]);
			RGB15(16, Yblk[8]);
			RGB15(17, Yblk[9]);

			Cr = *(Crblk+4);
			Cb = *(Cbblk+4);
			R = MULR(Cr);
			G = MULG(Cb) + MULG2(Cr);
			B = MULB(Cb);

			RGB15(8, Yblk[DCTSIZE2+0]);
			RGB15(9, Yblk[DCTSIZE2+1]);
			RGB15(24, Yblk[DCTSIZE2+8]);
			RGB15(25, Yblk[DCTSIZE2+9]);
		}
	} else
	for (y=0;y<16;y+=2,Yblk+=8,image+=24) {
		if (y==8) Yblk+=DCTSIZE2;
		for (x=0;x<4;x++,image+=2,Yblk+=2) {
			RGB15BW(0, Yblk[0]);
			RGB15BW(1, Yblk[1]);
			RGB15BW(16, Yblk[8]);
			RGB15BW(17, Yblk[9]);

			RGB15BW(8, Yblk[DCTSIZE2+0]);
			RGB15BW(9, Yblk[DCTSIZE2+1]);
			RGB15BW(24, Yblk[DCTSIZE2+8]);
			RGB15BW(25, Yblk[DCTSIZE2+9]);
		}
	}
}

static void yuv2rgb24(int *blk,unsigned char *image)
{
	int x,y;
	int *Yblk = blk+DCTSIZE2*2;
	int Cb,Cr,R,G,B;
	int *Cbblk = blk;
	int *Crblk = blk+DCTSIZE2;

	if (!(Config.Mdec&0x1))
	for (y=0;y<16;y+=2,Crblk+=4,Cbblk+=4,Yblk+=8,image+=24*3) {
		if (y==8) Yblk+=DCTSIZE2;
		for (x=0;x<4;x++,image+=6,Crblk++,Cbblk++,Yblk+=2) {
			Cr = *Crblk;
			Cb = *Cbblk;
			R = MULR(Cr);
			G = MULG(Cb) + MULG2(Cr);
			B = MULB(Cb);

			RGB24(0, Yblk[0]);
			RGB24(1*3, Yblk[1]);
			RGB24(16*3, Yblk[8]);
			RGB24(17*3, Yblk[9]);

			Cr = *(Crblk+4);
			Cb = *(Cbblk+4);
			R = MULR(Cr);
			G = MULG(Cb) + MULG2(Cr);
			B = MULB(Cb);

			RGB24(8*3, Yblk[DCTSIZE2+0]);
			RGB24(9*3, Yblk[DCTSIZE2+1]);
			RGB24(24*3, Yblk[DCTSIZE2+8]);
			RGB24(25*3, Yblk[DCTSIZE2+9]);
		}
	} else
	for (y=0;y<16;y+=2,Yblk+=8,image+=24*3) {
		if (y==8) Yblk+=DCTSIZE2;
		for (x=0;x<4;x++,image+=6,Yblk+=2) {
			RGB24BW(0, Yblk[0]);
			RGB24BW(1*3, Yblk[1]);
			RGB24BW(16*3, Yblk[8]);
			RGB24BW(17*3, Yblk[9]);

			RGB24BW(8*3, Yblk[DCTSIZE2+0]);
			RGB24BW(9*3, Yblk[DCTSIZE2+1]);
			RGB24BW(24*3, Yblk[DCTSIZE2+8]);
			RGB24BW(25*3, Yblk[DCTSIZE2+9]);
		}
	}
}

static uint16_t* rl2blk(int *blk, uint16_t *mdec_rl)
{
	int i,k,q_scale,rl;
	int *iqtab;

	memset (blk, 0, 6*DCTSIZE2*4);
	iqtab = iq_uv;
	for(i=0;i<6;i++) {	// decode blocks (Cr,Cb,Y1,Y2,Y3,Y4)
		if (i>1) iqtab = iq_y;

		// zigzag transformation
		rl = *mdec_rl++;
		q_scale = RUNOF(rl);
		blk[0] = iqtab[0]*VALOF(rl);
		for(k = 0;;) {
			rl = *mdec_rl++;
			if (rl==NOP) break;
			k += RUNOF(rl)+1;	// skip level zero-coefficients
			if (k > 63) break;
			blk[zscan[k]] = (VALOF(rl) * iqtab[k] * q_scale) / 8; // / 16;
		}

		idct(blk,k+1);
		blk+=DCTSIZE2;
	}
	return mdec_rl;
}

void mdecInit(void)
{
	Config.Mdec  = 0; //XXXXXXXXXXXXXXXXX  0 or 1 // 1 is black and white decoding
	mdec.rl      = (u16*)PSXM(0);
	mdec.command = 0;
	mdec.status  = 0;
	round_init();
}


void mdecWrite0(u32 data)
{
	mdec.command = data;
	if ((data&0xf5ff0000)==0x30000000)
		mdec.rlsize = data&0xffff;
}

void mdecWrite1(u32 data)
{
	if (data&0x80000000) // mdec reset
		round_init();
}

u32 mdecRead0(void) { return mdec.command; }
u32 mdecRead1(void) { return mdec.status;  }

void psxDma0(u32 adr, u32 bcr, u32 chcr)
{
	int cmd = mdec.command;

	if (chcr != 0x01000201) return;

	// bcr LSBs are the blocksize in words
	// bcr MSBs are the number of block
	int size = (bcr >> 16)*(bcr & 0xffff);
	if (size < 0) { /* psxDma0 DMA transfer overflow */
		// Need to investigate what happen if the transfer is huge
		return;
	}

	for (int i = 0; i<(size); i++)
		*(u32*)PSXM(((i + 0) * 4)) = iopMemRead32(adr + ((i + 0) * 4));


	if (cmd == 0x40000001) {
		u8 *p = (u8*)PSXM(0); //u8 *p = (u8*)PSXM(adr);
		iqtab_init(iq_y, p);
		iqtab_init(iq_uv, p + 64);
	}
	else if ((cmd & 0xf5ff0000) == 0x30000000) {
		mdec.rl = (u16*)PSXM(0); //mdec.rl = (u16*)PSXM(adr);
	}

	HW_DMA0_CHCR &= ~0x01000000;
	psxDmaInterrupt(0);
}

void psxDma1(u32 adr, u32 bcr, u32 chcr)
{
	int blk[DCTSIZE2*6];
	uint16_t *image;

	if (chcr != 0x01000200) return;
	// bcr LSBs are the blocksize in words
	// bcr MSBs are the number of block
	int size = (bcr >> 16)*(bcr & 0xffff);
	int size2 = (bcr >> 16)*(bcr & 0xffff);
	if (size < 0) { /* psxDma1 DMA transfer overflow */
		// Need to investigate what happen if the transfer is huge
		return;
	}

	image = (u16*)mdecArr2;//(u16*)PSXM(0); //image = (u16*)PSXM(adr);

	if (mdec.command&0x08000000) {
		for (;size>0;size-=(16*16)/2,image+=(16*16)) {
			mdec.rl = rl2blk(blk,mdec.rl);
			yuv2rgb15(blk,image);
		}
	} else {
		for (;size>0;size-=(24*16)/2,image+=(24*16)) {
			mdec.rl = rl2blk(blk,mdec.rl);
			yuv2rgb24(blk,(u8 *)image);
		}
	}

	for (int i = 0; i<(size2); i++)
		iopMemWrite32(((adr & 0x00FFFFFF) + (i * 4) + 0), mdecArr2[i]);

	HW_DMA1_CHCR &= ~0x01000000;
	psxDmaInterrupt(1);
}

//todo: psxmode: add mdec savestate support
//int SaveState::mdecFreeze() {
//	Freeze(mdec);
//	Freeze(iq_y);
//	Freeze(iq_uv);
//
//	return 0;
//
//}
