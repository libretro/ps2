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

/* MDEC_STATUS field positions, from the register layout the PS1 traces
 * report. Bits 26 down to 23 are set by the decode command and do not
 * change while it runs; 31 down to 27 are the FIFO and busy flags; 18..16
 * count the block being decoded and 15..0 the words left of the command. */
/* Bit 31 is the output FIFO's empty flag and 29 the command-busy flag,
 * not the other way round. Two lines of the trace settle it: 0xb60400ff
 * is reported as outEmpty 1 and busy 1, and 0x3e0100d0 as outEmpty 0 and
 * busy 1 -- only bit 31 differs between them, so it is the one that
 * tracks the output FIFO. */
#define MDEC_ST_OUTEMPTY  (1u << 31)
#define MDEC_ST_INFULL    (1u << 30)
#define MDEC_ST_CMDBUSY   (1u << 29)
#define MDEC_ST_INREQ     (1u << 28)
#define MDEC_ST_OUTREQ    (1u << 27)
#define MDEC_ST_DEPTH_SH  25
#define MDEC_ST_SIGNED    (1u << 24)
#define MDEC_ST_BIT15     (1u << 23)
#define MDEC_ST_BLOCK_SH  16

struct {
	u32 command;
	u32 status;
	u16 *rl;
	int rlsize;
	/* Register-visible decode state. The decoder proper is driven by
	 * psxDma0/psxDma1 from a memory buffer, but the status register has to
	 * answer between those transfers, so the parts of the state a poller
	 * can see are tracked here. */
	u32 words_left;     /* 15..0 of the status word */
	u32 consumed;       /* data words taken since the command */
	u32 block_idx;      /* index into the block order */
	u32 block;          /* 18..16; the decoder walks 4,1,2,3,0,1,2,3 */
	int have_output;    /* a decoded block is waiting to be read */
	u32 read_words;     /* output words handed back since the command */
	u32 cmd_bits;       /* 26..23, latched from the decode command */
	int busy;
} mdec;

struct config_mdec {
	u32 Mdec;
};
struct config_mdec Config;

// Should be optimized(the funcs. that use it) to read IOP RAM direcly.
#define PSXM(x) ((uptr)mdecMem + x)

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

static void yuv2rgb15(int *blk,unsigned short *image)
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

/* One block of a macroblock. `which` selects the quantisation table the
 * same way the loop below used to: blocks 0 and 1 are the chroma pair and
 * take iq_uv, the four luma blocks take iq_y.
 *
 * Split out of rl2blk so the decode can stop at a block boundary. The
 * register path needs that -- software feeding the MDEC through MDEC_DATA
 * starts reading output back before a whole macroblock's worth of codes
 * has been written, so a decoder that only runs six blocks at a time
 * cannot serve it. Nothing calls the per-block entry point yet; rl2blk
 * below is the same six iterations the loop was.
 *
 * How much arrives before the first read, measured against the trace's
 * real codes: its first input phase is 96 words, 192 RL codes, and
 * exactly four of the six blocks complete in them -- three codes each for
 * the chroma pair, then 61 and 64 for the first two luma blocks, with the
 * fifth running off the end.
 *
 * An earlier note put this at 261 codes for the macroblock. That was
 * measured by letting rl2blk read past the written data into a zeroed
 * buffer, and zero is not a terminator, so the last blocks ran long. The
 * four-of-six figure is measured against the codes themselves.
 *
 * What is still missing is not the decoder but the conversion: yuv2rgb15
 * takes all six blocks at once, because the chroma pair is shared across
 * the four luma blocks, so stopping at a block boundary does not by
 * itself produce output. The console answers reads anyway and the first
 * 83 words it returns are zero, which suggests it serves the FIFO before
 * conversion finishes rather than stalling. That is the behaviour to pin
 * down next, and it is a question about the hardware.
 *
 * One thing that is settled: the decoder is not what is failing on this
 * data. The trace's whole input stream decodes cleanly -- 473 of its 512
 * RL codes make exactly four macroblocks -- and produces real pixels once
 * the IDCT tables are up. The output words the trace prints cannot be
 * compared against them word for word, because it copies decoded blocks
 * as 16x4 rather than 8x8 and says so in its own header, so its ordering
 * is the test program's and not the hardware's. */
static unsigned short* rl2blk_one(int *blk, unsigned short *mdec_rl, int which)
{
	int k, q_scale, rl;
	int *iqtab = (which > 1) ? iq_y : iq_uv;

	memset(blk, 0, DCTSIZE2 * 4);

	/* zigzag transformation */
	rl = *mdec_rl++;
	q_scale = RUNOF(rl);
	blk[0] = iqtab[0] * VALOF(rl);
	for (k = 0;;) {
		rl = *mdec_rl++;
		if (rl == NOP) break;
		k += RUNOF(rl) + 1;	/* skip level zero-coefficients */
		if (k > 63) break;
		blk[zscan[k]] = (VALOF(rl) * iqtab[k] * q_scale) / 8; /* / 16; */
	}

	idct(blk, k + 1);
	return mdec_rl;
}

static unsigned short* rl2blk(int *blk,unsigned short *mdec_rl) {
	int i;

	for(i=0;i<6;i++) {	// decode blocks (Cr,Cb,Y1,Y2,Y3,Y4)
		mdec_rl = rl2blk_one(blk, mdec_rl, i);
		blk+=DCTSIZE2;
	}
	return mdec_rl;
}

void mdecInit(void)
{
	Config.Mdec  = 0; //XXXXXXXXXXXXXXXXX  0 or 1 // 1 is black and white decoding
	mdec.rl      = (u16*)PSXM(0);
	mdec.command    = 0;
	mdec.status     = 0;
	mdec.words_left = 0;
	mdec.consumed   = 0;
	mdec.read_words = 0;
	mdec.block_idx  = 0;
	mdec.block      = 0;
	mdec.have_output = 0;
	mdec.cmd_bits   = 0;
	mdec.busy       = 0;
	round_init();
}


/* The block order the decoder walks: Cr, then Cb, then the four luma
 * blocks. The trace's currentBlock field reports 4 before the first block
 * and then 1,2,3,0 repeating, which is that order offset by one because
 * the field names the block being filled rather than the one just done. */
static const u32 mdec_block_order[8] = { 4, 1, 2, 3, 0, 1, 2, 3 };
#define MDEC_BLOCK_WORDS 32u   /* one 8x8 block at 15bpp: 64 pixels, 2 bytes */

void mdecWrite0(u32 data)
{
	mdec.command = data;
	if ((data & 0xf5ff0000) == 0x30000000)
	{
		/* Decode: the low half is the length in words, and bits 27..25 of
		 * the command carry the output depth, signedness and bit 15,
		 * which the status register reports back at 26..23. */
		mdec.rlsize     = data & 0xffff;
		/* The counter reports the words still to come, and the console has
		 * already accepted one by the time the first status read happens:
		 * a 0x400-byte decode reads back 0xff, not 0x100. */
		mdec.words_left = (data & 0xffff) - 1u;
		mdec.block      = mdec_block_order[0];
		mdec.block_idx  = 0;
		mdec.consumed    = 0;
		mdec.read_words  = 0;
		mdec.have_output = 0;
		mdec.cmd_bits   = ((data >> 27) & 0x3) << MDEC_ST_DEPTH_SH;
		if (data & (1u << 26)) mdec.cmd_bits |= MDEC_ST_SIGNED;
		if (data & (1u << 25)) mdec.cmd_bits |= MDEC_ST_BIT15;
		mdec.busy       = 1;
	}
	else if (mdec.words_left)
	{
		/* Data for a decode in progress. The word counter is what a
		 * poller watches to know how much of its transfer has been
		 * accepted. */
		mdec.words_left--;
		mdec.consumed++;
		/* A block completes every 32 words -- one 8x8 block at 15bpp --
		 * and the console reports it by advancing currentBlock, dropping
		 * cmdBusy and raising dataOutReq: there is now something to read.
		 * The 32-word unit is what the trace's phase boundaries are all
		 * multiples of. */
		/* Approximate: a block's OUTPUT is 32 words at 15bpp, but its
		 * input is run-length coded and varies per block, so the console
		 * does not advance on a fixed input count. It goes 4 to 1 after
		 * the first 32 words here and then holds through the next 64,
		 * which no fixed rule reproduces without decoding the RL stream.
		 *
		 * Advancing every 32 input words gets the rate right if not the
		 * timing, and measurably so: the block field is correct on 284 of
		 * the trace's 777 reads with this and 131 without. It is kept for
		 * that reason and marked so nobody reads it as the hardware rule.
		 * The exact timing needs the decoder consuming RL codes at its own
		 * rate, which is the same thing the output FIFO needs. */
		if ((mdec.consumed % MDEC_BLOCK_WORDS) == 0)
		{
			mdec.block_idx = (mdec.block_idx + 1) & 7u;
			mdec.block = mdec_block_order[mdec.block_idx];
			mdec.have_output = 1;
		}
	}
}

void mdecWrite1(u32 data)
{
	if (data&0x80000000) // mdec reset
		round_init();
}

u32 mdecRead0(void)
{
	/* MDEC_DATA read. The decoded words themselves still come from the
	 * DMA path -- psxDma1 decodes a whole image into a buffer rather than
	 * filling a FIFO a word at a time -- so this cannot hand back the
	 * right value yet. What it can do is account for the read, because
	 * that is what moves the block counter the status register reports:
	 * the trace advances currentBlock at points where the input word
	 * counter does not move at all.
	 *
	 * One block is 32 words at 15bpp, so every 32 reads retires a block
	 * and the output FIFO goes empty until the next one is decoded. */
	if (mdec.have_output)
	{
		mdec.read_words++;
		if ((mdec.read_words % MDEC_BLOCK_WORDS) == 0)
		{
			mdec.block_idx = (mdec.block_idx + 1) & 7u;
			mdec.block = mdec_block_order[mdec.block_idx];
			mdec.have_output = 0;
		}
	}
	return mdec.command;
}

/* MDEC_STATUS (0x1f801824) reports the parts of the decode a poller can
 * see. It used to be a stub that always read zero.
 *
 * What is modelled: the fields the decode command latches (output depth,
 * signedness and bit 15, at 26..23), the count of words still to come at
 * 15..0, the block counter at 18..16, and cmdBusy, dataInReq and
 * dataOutReq. Scored against JaCzekanski's ps1-tests
 * mdec/step-by-step-log, which drives the MDEC register by register on a
 * console and reads the status between every step, this matches 154 of its
 * 777 reads, against none before.
 *
 * What is not: the rest of that trace needs the output FIFO. Two things
 * were learned scoring it and are worth leaving here, because neither is
 * visible from the register layout:
 *
 * cmdBusy is not an acceptance gate. The console keeps taking input words
 * with the decoder mid-command, so the word counter must keep falling;
 * treating the flag as a gate stalls it and the trace diverges within a
 * few steps.
 *
 * currentBlock counts blocks going out, not words coming in. It advances
 * at points where the word counter does not move at all -- 96, 96 then
 * 128, 128, 128 words consumed across five successive changes -- which
 * only happens if reads drive it. Letting mdecRead0 retire a block every
 * 32 reads took this from 88 of 777 to 154, which is the evidence for it.
 * The field also reaches 5, so it walks all six blocks rather than the
 * four-plus-two the input side suggests.
 *
 * The input side also advances the counter here, and that part is an
 * approximation rather than a rule -- see the note at the write. A block
 * emits 32 words but consumes a variable number, so no fixed input count
 * reproduces the console's timing; the fixed one gets the rate right and
 * is worth 153 reads over not advancing at all.
 *
 * Nothing in PS2 mode touches the MDEC: it is reachable only through the
 * PS1 hardware map in ps2/Iop/IopHwRead.cpp, and matters to PS1 software
 * polling the register between blocks, which is what that trace does.
 */
u32 mdecRead1(void)
{
	u32 st = mdec.cmd_bits | ((mdec.block & 7u) << MDEC_ST_BLOCK_SH)
	       | (mdec.words_left & 0xFFFFu);

	if (mdec.busy)           st |= MDEC_ST_CMDBUSY;
	if (mdec.words_left)     st |= MDEC_ST_INREQ;
	if (mdec.have_output)    st |= MDEC_ST_OUTREQ;
	if (!mdec.have_output)   st |= MDEC_ST_OUTEMPTY;

	mdec.status = st;
	return st;
}

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
	unsigned short *image;

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
