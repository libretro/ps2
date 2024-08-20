/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <cstring> /* memset */
#include <limits.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#if defined(_M_ARM64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#define MULHI16(a, b) vshrq_n_s16(vqdmulhq_s16((a), (b)), 1)
#endif

#ifdef _MSC_VER
#define BigEndian(in) _byteswap_ulong(in)
#define BigEndian64(in) _byteswap_uint64(in)
#define VLC_ALIGNED16 __declspec(align(16))
#else
#define BigEndian(in) __builtin_bswap32(in)   /* or we could use the asm function bswap... */
#define BigEndian64(in) __builtin_bswap64(in) /* or we could use the asm function bswap... */
#define VLC_ALIGNED16 __attribute__((aligned(16)))
#endif

#include "common/VectorIntrin.h"

#include "../Common.h"
#include "../Config.h"

#include "IPU.h"
#include "IPUdma.h"
#include "../GS/MultiISA.h"

struct macroblock_8
{
	u8 Y[16][16];		/* 0 */
	u8 Cb[8][8];		/* 1 */
	u8 Cr[8][8];		/* 2 */
};

struct macroblock_16
{
	s16 Y[16][16];		/* 0 */
	s16 Cb[8][8];		/* 1 */
	s16 Cr[8][8];		/* 2 */
};

struct macroblock_rgb32
{
	struct
	{
		u8 r, g, b, a;
	} c[16][16];
};

struct rgb16_t
{
	u16 r:5, g:5, b:5, a:1;
};

struct macroblock_rgb16
{
	rgb16_t	c[16][16];
};

struct decoder_t
{
	/* first, state that carries information from one macroblock to the */
	/* next inside a slice, and is never used outside of mpeg2_slice() */

	/* DCT coefficients - should be kept aligned ! */
	s16 DCTblock[64];

	u8 niq[64];		/* non-intraquant matrix (sequence header) */
	u8 iq[64];		/* intraquant matrix (sequence header) */

	macroblock_8 mb8;
	macroblock_16 mb16;
	macroblock_rgb32 rgb32;
	macroblock_rgb16 rgb16;

	uint ipu0_data;		/* amount of data in the output macroblock (in QWC) */
	uint ipu0_idx;

	int quantizer_scale;

	/* now non-slice-specific information */

	/* picture header stuff */

	/* what type of picture this is (I, P, B, D) */
	int coding_type;

	/* picture coding extension stuff */

	/* predictor for DC coefficients in intra blocks */
	s16 dc_dct_pred[3];

	/* quantization factor for intra dc coefficients */
	int intra_dc_precision;
	/* top/bottom/both fields */
	int picture_structure;
	/* bool to indicate all predictions are frame based */
	int frame_pred_frame_dct;
	/* bool to indicate whether intra blocks have motion vectors */
	/* (for concealment) */
	int concealment_motion_vectors;
	/* bit to indicate which quantization table to use */
	int q_scale_type;
	/* bool to use different vlc tables */
	int intra_vlc_format;
	/* used for DMV MC */
	int top_field_first;
	/* Pseudo Sign Offset */
	int sgn;
	int dte;			/* Dither Enable */
	int ofm;			/* Output Format */
	int macroblock_modes;		/* Macroblock type */
	int dcr;			/* DC Reset */
	int coded_block_pattern;	/* Coded block pattern */

	/* stuff derived from bitstream */

	/* the zigzag scan we're supposed to be using, true for alt, false for normal */
	bool scantype;

	int mpeg1;

	template< typename T >
	void SetOutputTo( T& obj )
	{
		uint mb_offset = ((uintptr_t)&obj - (uintptr_t)&mb8);
		ipu0_idx	= mb_offset / 16;
		ipu0_data	= sizeof(obj)/16;
	}

	u128* GetIpuDataPtr()
	{
		return ((u128*)&mb8) + ipu0_idx;
	}

	void AdvanceIpuDataBy(uint amt)
	{
		ipu0_idx  += amt;
		ipu0_data -= amt;
	}
};

enum macroblock_modes
{
	MACROBLOCK_INTRA = 1,
	MACROBLOCK_PATTERN = 2,
	MACROBLOCK_MOTION_BACKWARD = 4,
	MACROBLOCK_MOTION_FORWARD = 8,
	MACROBLOCK_QUANT = 16,
	DCT_TYPE_INTERLACED = 32
};

enum motion_type
{
	MOTION_TYPE_SHIFT = 6,
	MOTION_TYPE_MASK = (3 * 64),
	MOTION_TYPE_BASE = 64,
	MC_FIELD = (1 * 64),
	MC_FRAME = (2 * 64),
	MC_16X8 = (2 * 64),
	MC_DMV = (3 * 64)
};

/* picture structure */
enum picture_structure
{
	TOP_FIELD = 1,
	BOTTOM_FIELD = 2,
	FRAME_PICTURE = 3
};

/* picture coding type */
enum picture_coding_type
{
	I_TYPE = 1,
	P_TYPE = 2,
	B_TYPE = 3,
	D_TYPE = 4
};

struct MBtab
{
	uint8_t modes;
	uint8_t len;
};

struct MVtab
{
	uint8_t delta;
	uint8_t len;
};

struct DMVtab
{
	int8_t dmv;
	uint8_t len;
};

struct CBPtab
{
	uint8_t cbp;
	uint8_t len;
};

struct DCtab
{
	uint8_t size;
	uint8_t len;
};

struct DCTtab
{
	uint8_t run;
	uint8_t level;
	uint8_t len;
};

struct MBAtab
{
	uint8_t mba;
	uint8_t len;
};


#define INTRA MACROBLOCK_INTRA
#define QUANT MACROBLOCK_QUANT

static constexpr MBtab MB_I[] = {
	{INTRA | QUANT, 2}, {INTRA, 1}};

#define MC MACROBLOCK_MOTION_FORWARD
#define CODED MACROBLOCK_PATTERN

static constexpr VLC_ALIGNED16 MBtab MB_P[] = {
	{INTRA | QUANT, 6}, {CODED | QUANT, 5}, {MC | CODED | QUANT, 5}, {INTRA, 5},
	{MC, 3}, {MC, 3}, {MC, 3}, {MC, 3},
	{CODED, 2}, {CODED, 2}, {CODED, 2}, {CODED, 2},
	{CODED, 2}, {CODED, 2}, {CODED, 2}, {CODED, 2},
	{MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1},
	{MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1},
	{MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1},
	{MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}, {MC | CODED, 1}};

#define FWD MACROBLOCK_MOTION_FORWARD
#define BWD MACROBLOCK_MOTION_BACKWARD
#define INTER MACROBLOCK_MOTION_FORWARD | MACROBLOCK_MOTION_BACKWARD

static constexpr VLC_ALIGNED16 MBtab MB_B[] = {
	{0, 0}, {INTRA | QUANT, 6},
	{BWD | CODED | QUANT, 6}, {FWD | CODED | QUANT, 6},
	{INTER | CODED | QUANT, 5}, {INTER | CODED | QUANT, 5},
	{INTRA, 5}, {INTRA, 5},
	{FWD, 4}, {FWD, 4}, {FWD, 4}, {FWD, 4},
	{FWD | CODED, 4}, {FWD | CODED, 4}, {FWD | CODED, 4}, {FWD | CODED, 4},
	{BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3},
	{BWD, 3}, {BWD, 3}, {BWD, 3}, {BWD, 3},
	{BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3},
	{BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3}, {BWD | CODED, 3},
	{INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
	{INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
	{INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
	{INTER, 2}, {INTER, 2}, {INTER, 2}, {INTER, 2},
	{INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2},
	{INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2},
	{INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2},
	{INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}, {INTER | CODED, 2}};

#undef INTRA
#undef QUANT
#undef MC
#undef CODED
#undef FWD
#undef BWD
#undef INTER


static constexpr MVtab MV_4[] = {
	{3, 6}, {2, 4}, {1, 3}, {1, 3}, {0, 2}, {0, 2}, {0, 2}, {0, 2}};

static constexpr VLC_ALIGNED16 MVtab MV_10[] = {
	{0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10}, {0, 10},
	{0, 10}, {0, 10}, {0, 10}, {0, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10},
	{11, 10}, {10, 10}, {9, 9}, {9, 9}, {8, 9}, {8, 9}, {7, 9}, {7, 9},
	{6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7}, {6, 7},
	{5, 7}, {5, 7}, {5, 7}, {5, 7}, {5, 7}, {5, 7}, {5, 7}, {5, 7},
	{4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}, {4, 7}};


static constexpr DMVtab DMV_2[] = {
	{0, 1}, {0, 1}, {1, 2}, {-1, 2}};


static constexpr VLC_ALIGNED16 CBPtab CBP_7[] = {
	{0x22, 7}, {0x12, 7}, {0x0a, 7}, {0x06, 7},
	{0x21, 7}, {0x11, 7}, {0x09, 7}, {0x05, 7},
	{0x3f, 6}, {0x3f, 6}, {0x03, 6}, {0x03, 6},
	{0x24, 6}, {0x24, 6}, {0x18, 6}, {0x18, 6},
	{0x3e, 5}, {0x3e, 5}, {0x3e, 5}, {0x3e, 5},
	{0x02, 5}, {0x02, 5}, {0x02, 5}, {0x02, 5},
	{0x3d, 5}, {0x3d, 5}, {0x3d, 5}, {0x3d, 5},
	{0x01, 5}, {0x01, 5}, {0x01, 5}, {0x01, 5},
	{0x38, 5}, {0x38, 5}, {0x38, 5}, {0x38, 5},
	{0x34, 5}, {0x34, 5}, {0x34, 5}, {0x34, 5},
	{0x2c, 5}, {0x2c, 5}, {0x2c, 5}, {0x2c, 5},
	{0x1c, 5}, {0x1c, 5}, {0x1c, 5}, {0x1c, 5},
	{0x28, 5}, {0x28, 5}, {0x28, 5}, {0x28, 5},
	{0x14, 5}, {0x14, 5}, {0x14, 5}, {0x14, 5},
	{0x30, 5}, {0x30, 5}, {0x30, 5}, {0x30, 5},
	{0x0c, 5}, {0x0c, 5}, {0x0c, 5}, {0x0c, 5},
	{0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4},
	{0x20, 4}, {0x20, 4}, {0x20, 4}, {0x20, 4},
	{0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4},
	{0x10, 4}, {0x10, 4}, {0x10, 4}, {0x10, 4},
	{0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4},
	{0x08, 4}, {0x08, 4}, {0x08, 4}, {0x08, 4},
	{0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4},
	{0x04, 4}, {0x04, 4}, {0x04, 4}, {0x04, 4},
	{0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3},
	{0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3},
	{0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3},
	{0x3c, 3}, {0x3c, 3}, {0x3c, 3}, {0x3c, 3}};

static constexpr VLC_ALIGNED16 CBPtab CBP_9[] = {
	{0, 0}, {0x00, 9}, {0x27, 9}, {0x1b, 9},
	{0x3b, 9}, {0x37, 9}, {0x2f, 9}, {0x1f, 9},
	{0x3a, 8}, {0x3a, 8}, {0x36, 8}, {0x36, 8},
	{0x2e, 8}, {0x2e, 8}, {0x1e, 8}, {0x1e, 8},
	{0x39, 8}, {0x39, 8}, {0x35, 8}, {0x35, 8},
	{0x2d, 8}, {0x2d, 8}, {0x1d, 8}, {0x1d, 8},
	{0x26, 8}, {0x26, 8}, {0x1a, 8}, {0x1a, 8},
	{0x25, 8}, {0x25, 8}, {0x19, 8}, {0x19, 8},
	{0x2b, 8}, {0x2b, 8}, {0x17, 8}, {0x17, 8},
	{0x33, 8}, {0x33, 8}, {0x0f, 8}, {0x0f, 8},
	{0x2a, 8}, {0x2a, 8}, {0x16, 8}, {0x16, 8},
	{0x32, 8}, {0x32, 8}, {0x0e, 8}, {0x0e, 8},
	{0x29, 8}, {0x29, 8}, {0x15, 8}, {0x15, 8},
	{0x31, 8}, {0x31, 8}, {0x0d, 8}, {0x0d, 8},
	{0x23, 8}, {0x23, 8}, {0x13, 8}, {0x13, 8},
	{0x0b, 8}, {0x0b, 8}, {0x07, 8}, {0x07, 8}};

struct MBAtabSet
{
	MBAtab mba5[30];
	MBAtab mba11[26 * 4];
};
static constexpr VLC_ALIGNED16 MBAtabSet MBA = {
	{// mba5
		{6, 5}, {5, 5}, {4, 4}, {4, 4}, {3, 4}, {3, 4},
		{2, 3}, {2, 3}, {2, 3}, {2, 3}, {1, 3}, {1, 3}, {1, 3}, {1, 3},
		{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1},
		{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}},

	{// mba11
		{32, 11}, {31, 11}, {30, 11}, {29, 11},
		{28, 11}, {27, 11}, {26, 11}, {25, 11},
		{24, 11}, {23, 11}, {22, 11}, {21, 11},
		{20, 10}, {20, 10}, {19, 10}, {19, 10},
		{18, 10}, {18, 10}, {17, 10}, {17, 10},
		{16, 10}, {16, 10}, {15, 10}, {15, 10},
		{14, 8}, {14, 8}, {14, 8}, {14, 8},
		{14, 8}, {14, 8}, {14, 8}, {14, 8},
		{13, 8}, {13, 8}, {13, 8}, {13, 8},
		{13, 8}, {13, 8}, {13, 8}, {13, 8},
		{12, 8}, {12, 8}, {12, 8}, {12, 8},
		{12, 8}, {12, 8}, {12, 8}, {12, 8},
		{11, 8}, {11, 8}, {11, 8}, {11, 8},
		{11, 8}, {11, 8}, {11, 8}, {11, 8},
		{10, 8}, {10, 8}, {10, 8}, {10, 8},
		{10, 8}, {10, 8}, {10, 8}, {10, 8},
		{9, 8}, {9, 8}, {9, 8}, {9, 8},
		{9, 8}, {9, 8}, {9, 8}, {9, 8},
		{8, 7}, {8, 7}, {8, 7}, {8, 7},
		{8, 7}, {8, 7}, {8, 7}, {8, 7},
		{8, 7}, {8, 7}, {8, 7}, {8, 7},
		{8, 7}, {8, 7}, {8, 7}, {8, 7},
		{7, 7}, {7, 7}, {7, 7}, {7, 7},
		{7, 7}, {7, 7}, {7, 7}, {7, 7},
		{7, 7}, {7, 7}, {7, 7}, {7, 7},
		{7, 7}, {7, 7}, {7, 7}, {7, 7}}};

struct DCtabSet
{
	DCtab lum0[32]; // Table B-12, dct_dc_size_luminance, codes 00xxx ... 11110
	DCtab lum1[16]; // Table B-12, dct_dc_size_luminance, codes 111110xxx ... 111111111
	DCtab chrom0[32]; // Table B-13, dct_dc_size_chrominance, codes 00xxx ... 11110
	DCtab chrom1[32]; // Table B-13, dct_dc_size_chrominance, codes 111110xxxx ... 1111111111
};

static constexpr VLC_ALIGNED16 DCtabSet DCtable =
	{
		// lum0: Table B-12, dct_dc_size_luminance, codes 00xxx ... 11110 */
		{{1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2},
			{2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
			{0, 3}, {0, 3}, {0, 3}, {0, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3},
			{4, 3}, {4, 3}, {4, 3}, {4, 3}, {5, 4}, {5, 4}, {6, 5}, {0, 0}},

		/* lum1: Table B-12, dct_dc_size_luminance, codes 111110xxx ... 111111111 */
		{{7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6}, {7, 6},
			{8, 7}, {8, 7}, {8, 7}, {8, 7}, {9, 8}, {9, 8}, {10, 9}, {11, 9}},

		/* chrom0: Table B-13, dct_dc_size_chrominance, codes 00xxx ... 11110 */
		{{0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2}, {0, 2},
			{1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2}, {1, 2},
			{2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},
			{3, 3}, {3, 3}, {3, 3}, {3, 3}, {4, 4}, {4, 4}, {5, 5}, {0, 0}},

		/* chrom1: Table B-13, dct_dc_size_chrominance, codes 111110xxxx ... 1111111111 */
		{{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
			{6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6}, {6, 6},
			{7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {7, 7},
			{8, 8}, {8, 8}, {8, 8}, {8, 8}, {9, 9}, {9, 9}, {10, 10}, {11, 10}},
};

struct DCTtabSet
{
	DCTtab first[12];
	DCTtab next[12];

	DCTtab tab0[60];
	DCTtab tab0a[252];
	DCTtab tab1[8];
	DCTtab tab1a[8];

	DCTtab tab2[16];
	DCTtab tab3[16];
	DCTtab tab4[16];
	DCTtab tab5[16];
	DCTtab tab6[16];
};

static constexpr VLC_ALIGNED16 DCTtabSet DCT =
	{
		/* first[12]: Table B-14, DCT coefficients table zero,
	 * codes 0100 ... 1xxx (used for first (DC) coefficient)
	 */
		{{0, 2, 4}, {2, 1, 4}, {1, 1, 3}, {1, 1, 3},
			{0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {0, 1, 1},
			{0, 1, 1}, {0, 1, 1}, {0, 1, 1}, {0, 1, 1}},

		/* next[12]: Table B-14, DCT coefficients table zero,
	 * codes 0100 ... 1xxx (used for all other coefficients)
	 */
		{{0, 2, 4}, {2, 1, 4}, {1, 1, 3}, {1, 1, 3},
			{64, 0, 2}, {64, 0, 2}, {64, 0, 2}, {64, 0, 2}, /* EOB */
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2}},

		/* tab0[60]: Table B-14, DCT coefficients table zero,
	 * codes 000001xx ... 00111xxx
	 */
		{{65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, /* Escape */
			{2, 2, 7}, {2, 2, 7}, {9, 1, 7}, {9, 1, 7},
			{0, 4, 7}, {0, 4, 7}, {8, 1, 7}, {8, 1, 7},
			{7, 1, 6}, {7, 1, 6}, {7, 1, 6}, {7, 1, 6},
			{6, 1, 6}, {6, 1, 6}, {6, 1, 6}, {6, 1, 6},
			{1, 2, 6}, {1, 2, 6}, {1, 2, 6}, {1, 2, 6},
			{5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {5, 1, 6},
			{13, 1, 8}, {0, 6, 8}, {12, 1, 8}, {11, 1, 8},
			{3, 2, 8}, {1, 3, 8}, {0, 5, 8}, {10, 1, 8},
			{0, 3, 5}, {0, 3, 5}, {0, 3, 5}, {0, 3, 5},
			{0, 3, 5}, {0, 3, 5}, {0, 3, 5}, {0, 3, 5},
			{4, 1, 5}, {4, 1, 5}, {4, 1, 5}, {4, 1, 5},
			{4, 1, 5}, {4, 1, 5}, {4, 1, 5}, {4, 1, 5},
			{3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5},
			{3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5}},

		/* tab0a[252]: Table B-15, DCT coefficients table one,
	 * codes 000001xx ... 11111111
	 */
		{{65, 0, 6}, {65, 0, 6}, {65, 0, 6}, {65, 0, 6}, /* Escape */
			{7, 1, 7}, {7, 1, 7}, {8, 1, 7}, {8, 1, 7},
			{6, 1, 7}, {6, 1, 7}, {2, 2, 7}, {2, 2, 7},
			{0, 7, 6}, {0, 7, 6}, {0, 7, 6}, {0, 7, 6},
			{0, 6, 6}, {0, 6, 6}, {0, 6, 6}, {0, 6, 6},
			{4, 1, 6}, {4, 1, 6}, {4, 1, 6}, {4, 1, 6},
			{5, 1, 6}, {5, 1, 6}, {5, 1, 6}, {5, 1, 6},
			{1, 5, 8}, {11, 1, 8}, {0, 11, 8}, {0, 10, 8},
			{13, 1, 8}, {12, 1, 8}, {3, 2, 8}, {1, 4, 8},
			{2, 1, 5}, {2, 1, 5}, {2, 1, 5}, {2, 1, 5},
			{2, 1, 5}, {2, 1, 5}, {2, 1, 5}, {2, 1, 5},
			{1, 2, 5}, {1, 2, 5}, {1, 2, 5}, {1, 2, 5},
			{1, 2, 5}, {1, 2, 5}, {1, 2, 5}, {1, 2, 5},
			{3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5},
			{3, 1, 5}, {3, 1, 5}, {3, 1, 5}, {3, 1, 5},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{1, 1, 3}, {1, 1, 3}, {1, 1, 3}, {1, 1, 3},
			{64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4}, /* EOB */
			{64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
			{64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
			{64, 0, 4}, {64, 0, 4}, {64, 0, 4}, {64, 0, 4},
			{0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
			{0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
			{0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
			{0, 3, 4}, {0, 3, 4}, {0, 3, 4}, {0, 3, 4},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 1, 2}, {0, 1, 2}, {0, 1, 2}, {0, 1, 2},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 2, 3}, {0, 2, 3}, {0, 2, 3}, {0, 2, 3},
			{0, 4, 5}, {0, 4, 5}, {0, 4, 5}, {0, 4, 5},
			{0, 4, 5}, {0, 4, 5}, {0, 4, 5}, {0, 4, 5},
			{0, 5, 5}, {0, 5, 5}, {0, 5, 5}, {0, 5, 5},
			{0, 5, 5}, {0, 5, 5}, {0, 5, 5}, {0, 5, 5},
			{9, 1, 7}, {9, 1, 7}, {1, 3, 7}, {1, 3, 7},
			{10, 1, 7}, {10, 1, 7}, {0, 8, 7}, {0, 8, 7},
			{0, 9, 7}, {0, 9, 7}, {0, 12, 8}, {0, 13, 8},
			{2, 3, 8}, {4, 2, 8}, {0, 14, 8}, {0, 15, 8}},

		/* Table B-14, DCT coefficients table zero,
	 * codes 0000001000 ... 0000001111
	 */
		{{16, 1, 10}, {5, 2, 10}, {0, 7, 10}, {2, 3, 10},
			{1, 4, 10}, {15, 1, 10}, {14, 1, 10}, {4, 2, 10}},

		/* Table B-15, DCT coefficients table one,
	 * codes 000000100x ... 000000111x
	 */
		{{5, 2, 9}, {5, 2, 9}, {14, 1, 9}, {14, 1, 9},
			{2, 4, 10}, {16, 1, 10}, {15, 1, 9}, {15, 1, 9}},

		/* Table B-14/15, DCT coefficients table zero / one,
	 * codes 000000010000 ... 000000011111
	 */
		{{0, 11, 12}, {8, 2, 12}, {4, 3, 12}, {0, 10, 12},
			{2, 4, 12}, {7, 2, 12}, {21, 1, 12}, {20, 1, 12},
			{0, 9, 12}, {19, 1, 12}, {18, 1, 12}, {1, 5, 12},
			{3, 3, 12}, {0, 8, 12}, {6, 2, 12}, {17, 1, 12}},

		/* Table B-14/15, DCT coefficients table zero / one,
	 * codes 0000000010000 ... 0000000011111
	 */
		{{10, 2, 13}, {9, 2, 13}, {5, 3, 13}, {3, 4, 13},
			{2, 5, 13}, {1, 7, 13}, {1, 6, 13}, {0, 15, 13},
			{0, 14, 13}, {0, 13, 13}, {0, 12, 13}, {26, 1, 13},
			{25, 1, 13}, {24, 1, 13}, {23, 1, 13}, {22, 1, 13}},

		/* Table B-14/15, DCT coefficients table zero / one,
	 * codes 00000000010000 ... 00000000011111
	 */
		{{0, 31, 14}, {0, 30, 14}, {0, 29, 14}, {0, 28, 14},
			{0, 27, 14}, {0, 26, 14}, {0, 25, 14}, {0, 24, 14},
			{0, 23, 14}, {0, 22, 14}, {0, 21, 14}, {0, 20, 14},
			{0, 19, 14}, {0, 18, 14}, {0, 17, 14}, {0, 16, 14}},

		/* Table B-14/15, DCT coefficients table zero / one,
	 * codes 000000000010000 ... 000000000011111
	 */
		{{0, 40, 15}, {0, 39, 15}, {0, 38, 15}, {0, 37, 15},
			{0, 36, 15}, {0, 35, 15}, {0, 34, 15}, {0, 33, 15},
			{0, 32, 15}, {1, 14, 15}, {1, 13, 15}, {1, 12, 15},
			{1, 11, 15}, {1, 10, 15}, {1, 9, 15}, {1, 8, 15}},

		/* Table B-14/15, DCT coefficients table zero / one,
	 * codes 0000000000010000 ... 0000000000011111
	 */
		{{1, 18, 16}, {1, 17, 16}, {1, 16, 16}, {1, 15, 16},
			{6, 3, 16}, {16, 2, 16}, {15, 2, 16}, {14, 2, 16},
			{13, 2, 16}, {12, 2, 16}, {11, 2, 16}, {31, 1, 16},
			{30, 1, 16}, {29, 1, 16}, {28, 1, 16}, {27, 1, 16}}

};

MULTI_ISA_DEF(
	extern void ipu_dither(const macroblock_rgb32& rgb32, macroblock_rgb16& rgb16, const int dte);

	void IPUWorker();
)

alignas(16) extern decoder_t decoder;
alignas(16) extern tIPU_BP g_BP;

/* Quantization matrix */
extern rgb16_t g_ipu_vqclut[16]; /* CLUT conversion table */
extern u16 g_ipu_thresh[2];      /* Thresholds for color conversions */

alignas(16) extern u8 g_ipu_indx4[16*16/2];
alignas(16) extern const int non_linear_quantizer_scale[32];
extern int coded_block_pattern;

struct mpeg2_scan_pack
{
	u8 norm[64];
	u8 alt[64];
};

alignas(16) extern const std::array<u8, 1024> g_idct_clip_lut;
alignas(16) extern const mpeg2_scan_pack mpeg2_scan;

MULTI_ISA_DEF(extern void yuv2rgb(void);)

static IPUregisters& ipuRegs = (IPUregisters&)eeHw[0x2000];

/* the BP doesn't advance and returns -1 if there is no data to be read */
alignas(16) tIPU_cmd ipu_cmd;
alignas(16) tIPU_BP g_BP;
alignas(16) decoder_t decoder;
IPUStatus IPUCoreStatus;

static void (*IPUWorker)(void);

/* Quantization matrix */
rgb16_t g_ipu_vqclut[16];    /* CLUT conversion table */
u16 g_ipu_thresh[2];         /* Thresholds for color conversions */
int coded_block_pattern = 0;

alignas(16) u8 g_ipu_indx4[16*16/2];

alignas(16) const int non_linear_quantizer_scale[32] =
{
	0,  1,  2,  3,  4,  5,	6,	7,
	8, 10, 12, 14, 16, 18,  20,  22,
	24, 28, 32, 36, 40, 44,  48,  52,
	56, 64, 72, 80, 88, 96, 104, 112
};

uint eecount_on_last_vdec = 0;
bool FMVstarted = false;
bool EnableFMV = false;

alignas(16) IPU_Fifo ipu_fifo;

/* The IPU is fixed to 16 byte strides (128-bit / QWC resolution): */
static const uint decoder_stride = 16;

IPUDMAStatus IPU1Status;

/* IPU-correct YUV conversions by Pseudonym
 * SSE2 Implementation by Pseudonym */

/* The IPU's colour space conversion conforms to ITU-R Recommendation BT.601 
 * if anyone wants to make a faster or "more accurate" implementation, but 
 * this is the precise documented integer method used by the hardware and 
 * is fast enough with SSE2. */

#define IPU_Y_BIAS    16
#define IPU_C_BIAS    128
#define IPU_Y_COEFF   0x95	/*  1.1640625 */
#define IPU_GCR_COEFF (-0x68)	/* -0.8125 */
#define IPU_GCB_COEFF (-0x32)	/* -0.390625 */
#define IPU_RCR_COEFF 0xcc	/*  1.59375 */
#define IPU_BCB_COEFF 0x102	/*  2.015625 */

MULTI_ISA_UNSHARED_START

__ri void ipu_dither(const macroblock_rgb32 &rgb32, macroblock_rgb16 &rgb16, const int dte)
{
#if _M_SSE >= 0x200 /* SSE2 codepath */
	const __m128i alpha_test = _mm_set1_epi16(0x40);
	if (dte)
	{
		const __m128i dither_add_matrix[] = {
			_mm_setr_epi32(0x00000000, 0x00000000, 0x00000000, 0x00010101),
			_mm_setr_epi32(0x00020202, 0x00000000, 0x00030303, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00000000),
			_mm_setr_epi32(0x00030303, 0x00000000, 0x00020202, 0x00000000),
		};
		const __m128i dither_sub_matrix[] = {
			_mm_setr_epi32(0x00040404, 0x00000000, 0x00030303, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00020202, 0x00000000, 0x00010101),
			_mm_setr_epi32(0x00030303, 0x00000000, 0x00040404, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00020202),
		};
		for (int i = 0; i < 16; ++i)
		{
			const __m128i dither_add = dither_add_matrix[i & 3];
			const __m128i dither_sub = dither_sub_matrix[i & 3];
			for (int n = 0; n < 2; ++n)
			{
				__m128i rgba_8_0123          = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8]));
				__m128i rgba_8_4567          = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8 + 4]));

				// Dither and clamp
				rgba_8_0123                  = _mm_adds_epu8(rgba_8_0123, dither_add);
				rgba_8_0123                  = _mm_subs_epu8(rgba_8_0123, dither_sub);
				rgba_8_4567                  = _mm_adds_epu8(rgba_8_4567, dither_add);
				rgba_8_4567                  = _mm_subs_epu8(rgba_8_4567, dither_sub);

				// Split into channel components and extend to 16 bits
				const __m128i rgba_16_0415   = _mm_unpacklo_epi8(rgba_8_0123, rgba_8_4567);
				const __m128i rgba_16_2637   = _mm_unpackhi_epi8(rgba_8_0123, rgba_8_4567);
				const __m128i rgba_32_0246   = _mm_unpacklo_epi8(rgba_16_0415, rgba_16_2637);
				const __m128i rgba_32_1357   = _mm_unpackhi_epi8(rgba_16_0415, rgba_16_2637);
				const __m128i rg_64_01234567 = _mm_unpacklo_epi8(rgba_32_0246, rgba_32_1357);
				const __m128i ba_64_01234567 = _mm_unpackhi_epi8(rgba_32_0246, rgba_32_1357);

				const __m128i zero           = _mm_setzero_si128();
				__m128i r                    = _mm_unpacklo_epi8(rg_64_01234567, zero);
				__m128i g                    = _mm_unpackhi_epi8(rg_64_01234567, zero);
				__m128i b                    = _mm_unpacklo_epi8(ba_64_01234567, zero);
				__m128i a                    = _mm_unpackhi_epi8(ba_64_01234567, zero);

				// Create RGBA
				r                            = _mm_srli_epi16(r, 3);
				g                            = _mm_slli_epi16(_mm_srli_epi16(g, 3), 5);
				b                            = _mm_slli_epi16(_mm_srli_epi16(b, 3), 10);
				a                            = _mm_slli_epi16(_mm_cmpeq_epi16(a, alpha_test), 15);

				const __m128i rgba16         = _mm_or_si128(_mm_or_si128(r, g), _mm_or_si128(b, a));

				_mm_store_si128(reinterpret_cast<__m128i *>(&rgb16.c[i][n * 8]), rgba16);
			}
		}
	}
	else
	{
		for (int i = 0; i < 16; ++i)
		{
			for (int n = 0; n < 2; ++n)
			{
				__m128i rgba_8_0123          = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8]));
				__m128i rgba_8_4567          = _mm_load_si128(reinterpret_cast<const __m128i *>(&rgb32.c[i][n * 8 + 4]));
				// Split into channel components and extend to 16 bits
				const __m128i rgba_16_0415   = _mm_unpacklo_epi8(rgba_8_0123, rgba_8_4567);
				const __m128i rgba_16_2637   = _mm_unpackhi_epi8(rgba_8_0123, rgba_8_4567);
				const __m128i rgba_32_0246   = _mm_unpacklo_epi8(rgba_16_0415, rgba_16_2637);
				const __m128i rgba_32_1357   = _mm_unpackhi_epi8(rgba_16_0415, rgba_16_2637);
				const __m128i rg_64_01234567 = _mm_unpacklo_epi8(rgba_32_0246, rgba_32_1357);
				const __m128i ba_64_01234567 = _mm_unpackhi_epi8(rgba_32_0246, rgba_32_1357);

				const __m128i zero           = _mm_setzero_si128();
				__m128i r                    = _mm_unpacklo_epi8(rg_64_01234567, zero);
				__m128i g                    = _mm_unpackhi_epi8(rg_64_01234567, zero);
				__m128i b                    = _mm_unpacklo_epi8(ba_64_01234567, zero);
				__m128i a                    = _mm_unpackhi_epi8(ba_64_01234567, zero);

				// Create RGBA
				r                            = _mm_srli_epi16(r, 3);
				g                            = _mm_slli_epi16(_mm_srli_epi16(g, 3), 5);
				b                            = _mm_slli_epi16(_mm_srli_epi16(b, 3), 10);
				a                            = _mm_slli_epi16(_mm_cmpeq_epi16(a, alpha_test), 15);

				const __m128i rgba16         = _mm_or_si128(_mm_or_si128(r, g), _mm_or_si128(b, a));

				_mm_store_si128(reinterpret_cast<__m128i *>(&rgb16.c[i][n * 8]), rgba16);
			}
		}
	}
#else /* Reference C implementation */
	if (dte)
	{
		// I'm guessing values are rounded down when clamping.
		const int dither_coefficient[4][4] = {
			{-4, 0, -3, 1},
			{2, -2, 3, -1},
			{-3, 1, -4, 0},
			{3, -1, 2, -2},
		};
		for (int i = 0; i < 16; ++i)
		{
			for (int j = 0; j < 16; ++j)
			{
				const int dither = dither_coefficient[i & 3][j & 3];
				const int r = std::max(0, std::min(rgb32.c[i][j].r + dither, 255));
				const int g = std::max(0, std::min(rgb32.c[i][j].g + dither, 255));
				const int b = std::max(0, std::min(rgb32.c[i][j].b + dither, 255));

				rgb16.c[i][j].r = r >> 3;
				rgb16.c[i][j].g = g >> 3;
				rgb16.c[i][j].b = b >> 3;
				rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
			}
		}
	}
	else
	{
		for (int i = 0; i < 16; ++i)
		{
			for (int j = 0; j < 16; ++j)
			{
				rgb16.c[i][j].r = rgb32.c[i][j].r >> 3;
				rgb16.c[i][j].g = rgb32.c[i][j].g >> 3;
				rgb16.c[i][j].b = rgb32.c[i][j].b >> 3;
				rgb16.c[i][j].a = rgb32.c[i][j].a == 0x40;
			}
		}
	}
#endif
}

void yuv2rgb(void)
{
#if _M_SSE >= 0x200 /* SSE2 codepath */
	/* An AVX2 version is only slightly faster than an SSE2 version (+2-3fps)
	 * (or I'm a poor optimiser), though it might be worth attempting again
	 * once we've ported to 64 bits (the extra registers should help). */
	const __m128i c_bias = _mm_set1_epi8(s8(IPU_C_BIAS));
	const __m128i y_bias = _mm_set1_epi8(IPU_Y_BIAS);
	const __m128i y_mask = _mm_set1_epi16(s16(0xFF00));
	/* Specifying round off instead of round down as everywhere else
	 * implies that this is right */
	const __m128i round_1bit = _mm_set1_epi16(0x0001);;

	const __m128i y_coefficient = _mm_set1_epi16(s16(IPU_Y_COEFF << 2));
	const __m128i gcr_coefficient = _mm_set1_epi16(s16(u16(IPU_GCR_COEFF) << 2));
	const __m128i gcb_coefficient = _mm_set1_epi16(s16(u16(IPU_GCB_COEFF) << 2));
	const __m128i rcr_coefficient = _mm_set1_epi16(s16(IPU_RCR_COEFF << 2));
	const __m128i bcb_coefficient = _mm_set1_epi16(s16(IPU_BCB_COEFF << 2));

	/* Alpha set to 0x80 here. The threshold stuff is done later. */
	const __m128i& alpha = c_bias;

	for (int n = 0; n < 8; ++n)
	{
		/* Could skip the loadl_epi64 but most SSE instructions require 128-bit
		 * alignment so two versions would be needed. */
		__m128i cb = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cb[n][0]));
		__m128i cr = _mm_loadl_epi64(reinterpret_cast<__m128i*>(&decoder.mb8.Cr[n][0]));

		/* (Cb - 128) << 8, (Cr - 128) << 8 */
		cb = _mm_xor_si128(cb, c_bias);
		cr = _mm_xor_si128(cr, c_bias);
		cb = _mm_unpacklo_epi8(_mm_setzero_si128(), cb);
		cr = _mm_unpacklo_epi8(_mm_setzero_si128(), cr);

		__m128i rc = _mm_mulhi_epi16(cr, rcr_coefficient);
		__m128i gc = _mm_adds_epi16(_mm_mulhi_epi16(cr, gcr_coefficient), _mm_mulhi_epi16(cb, gcb_coefficient));
		__m128i bc = _mm_mulhi_epi16(cb, bcb_coefficient);

		for (int m = 0; m < 2; ++m) {
			__m128i y = _mm_load_si128(reinterpret_cast<__m128i*>(&decoder.mb8.Y[n * 2 + m][0]));
			y = _mm_subs_epu8(y, y_bias);
			/* Y << 8 for pixels 0, 2, 4, 6, 8, 10, 12, 14 */
			__m128i y_even = _mm_slli_epi16(y, 8);
			/* Y << 8 for pixels 1, 3, 5, 7 ,9, 11, 13, 15 */
			__m128i y_odd = _mm_and_si128(y, y_mask);

			y_even = _mm_mulhi_epu16(y_even, y_coefficient);
			y_odd  = _mm_mulhi_epu16(y_odd,  y_coefficient);

			__m128i r_even = _mm_adds_epi16(rc, y_even);
			__m128i r_odd  = _mm_adds_epi16(rc, y_odd);
			__m128i g_even = _mm_adds_epi16(gc, y_even);
			__m128i g_odd  = _mm_adds_epi16(gc, y_odd);
			__m128i b_even = _mm_adds_epi16(bc, y_even);
			__m128i b_odd  = _mm_adds_epi16(bc, y_odd);

			/* round */
			r_even = _mm_srai_epi16(_mm_add_epi16(r_even, round_1bit), 1);
			r_odd  = _mm_srai_epi16(_mm_add_epi16(r_odd,  round_1bit), 1);
			g_even = _mm_srai_epi16(_mm_add_epi16(g_even, round_1bit), 1);
			g_odd  = _mm_srai_epi16(_mm_add_epi16(g_odd,  round_1bit), 1);
			b_even = _mm_srai_epi16(_mm_add_epi16(b_even, round_1bit), 1);
			b_odd  = _mm_srai_epi16(_mm_add_epi16(b_odd,  round_1bit), 1);

			/* combine even and odd bytes in original order */
			__m128i r = _mm_packus_epi16(r_even, r_odd);
			__m128i g = _mm_packus_epi16(g_even, g_odd);
			__m128i b = _mm_packus_epi16(b_even, b_odd);

			r = _mm_unpacklo_epi8(r, _mm_shuffle_epi32(r, _MM_SHUFFLE(3, 2, 3, 2)));
			g = _mm_unpacklo_epi8(g, _mm_shuffle_epi32(g, _MM_SHUFFLE(3, 2, 3, 2)));
			b = _mm_unpacklo_epi8(b, _mm_shuffle_epi32(b, _MM_SHUFFLE(3, 2, 3, 2)));

			/* Create RGBA (we could generate A here, but we don't) quads */
			__m128i rg_l = _mm_unpacklo_epi8(r, g);
			__m128i ba_l = _mm_unpacklo_epi8(b, alpha);
			__m128i rgba_ll = _mm_unpacklo_epi16(rg_l, ba_l);
			__m128i rgba_lh = _mm_unpackhi_epi16(rg_l, ba_l);

			__m128i rg_h = _mm_unpackhi_epi8(r, g);
			__m128i ba_h = _mm_unpackhi_epi8(b, alpha);
			__m128i rgba_hl = _mm_unpacklo_epi16(rg_h, ba_h);
			__m128i rgba_hh = _mm_unpackhi_epi16(rg_h, ba_h);

			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][0]), rgba_ll);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][4]), rgba_lh);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][8]), rgba_hl);
			_mm_store_si128(reinterpret_cast<__m128i*>(&decoder.rgb32.c[n * 2 + m][12]), rgba_hh);
		}
	}
#elif defined(_M_ARM64) /* ARM64 codepath */
	const int8x16_t c_bias = vdupq_n_s8(s8(IPU_C_BIAS));
	const uint8x16_t y_bias = vdupq_n_u8(IPU_Y_BIAS);
	const int16x8_t y_mask = vdupq_n_s16(s16(0xFF00));
	/* Specifying round off instead of round down as
	 * as everywhere else implies that this is right */
	const int16x8_t round_1bit = vdupq_n_s16(0x0001);

	const int16x8_t y_coefficient = vdupq_n_s16(s16(IPU_Y_COEFF << 2));
	const int16x8_t gcr_coefficient = vdupq_n_s16(s16(u16(IPU_GCR_COEFF) << 2));
	const int16x8_t gcb_coefficient = vdupq_n_s16(s16(u16(IPU_GCB_COEFF) << 2));
	const int16x8_t rcr_coefficient = vdupq_n_s16(s16(IPU_RCR_COEFF << 2));
	const int16x8_t bcb_coefficient = vdupq_n_s16(s16(IPU_BCB_COEFF << 2));

	/* Alpha set to 0x80 here. The threshold stuff is done later. */
	const uint8x16_t alpha = vreinterpretq_u8_s8(c_bias);

	for (int n = 0; n < 8; ++n)
	{
		/* Could skip the loadl_epi64 but most SSE instructions require 128-bit
		 * alignment so two versions would be needed. */
		int8x16_t cb = vcombine_s8(vld1_s8(reinterpret_cast<s8*>(&decoder.mb8.Cb[n][0])), vdup_n_s8(0));
		int8x16_t cr = vcombine_s8(vld1_s8(reinterpret_cast<s8*>(&decoder.mb8.Cr[n][0])), vdup_n_s8(0));

		/* (Cb - 128) << 8, (Cr - 128) << 8 */
		cb = veorq_s8(cb, c_bias);
		cr = veorq_s8(cr, c_bias);
		cb = vzip1q_s8(vdupq_n_s8(0), cb);
		cr = vzip1q_s8(vdupq_n_s8(0), cr);

		int16x8_t rc = MULHI16(vreinterpretq_s16_s8(cr), rcr_coefficient);
		int16x8_t gc = vqaddq_s16(MULHI16(vreinterpretq_s16_s8(cr), gcr_coefficient), MULHI16(vreinterpretq_s16_s8(cb), gcb_coefficient));
		int16x8_t bc = MULHI16(vreinterpretq_s16_s8(cb), bcb_coefficient);

		for (int m = 0; m < 2; ++m)
		{
			uint8x16_t y = vld1q_u8(&decoder.mb8.Y[n * 2 + m][0]);
			y = vqsubq_u8(y, y_bias);
			/* Y << 8 for pixels 0, 2, 4, 6, 8, 10, 12, 14 */
			int16x8_t y_even = vshlq_n_s16(vreinterpretq_s16_u8(y), 8);
			/* Y << 8 for pixels 1, 3, 5, 7 ,9, 11, 13, 15 */
			int16x8_t y_odd = vandq_s16(vreinterpretq_s16_u8(y), y_mask);

#if 0
			y_even = _mm_mulhi_epu16(y_even, y_coefficient);
			y_odd = _mm_mulhi_epu16(y_odd, y_coefficient);
#endif

			uint16x4_t a3210 = vget_low_u16(vreinterpretq_u16_s16(y_even));
			uint16x4_t b3210 = vget_low_u16(vreinterpretq_u16_s16(y_coefficient));
			uint32x4_t ab3210 = vmull_u16(a3210, b3210);
			uint32x4_t ab7654 = vmull_high_u16(vreinterpretq_u16_s16(y_even), vreinterpretq_u16_s16(y_coefficient));
			y_even = vreinterpretq_s16_u16(vuzp2q_u16(vreinterpretq_u16_u32(ab3210), vreinterpretq_u16_u32(ab7654)));

			a3210 = vget_low_u16(vreinterpretq_u16_s16(y_odd));
			b3210 = vget_low_u16(vreinterpretq_u16_s16(y_coefficient));
			ab3210 = vmull_u16(a3210, b3210);
			ab7654 = vmull_high_u16(vreinterpretq_u16_s16(y_odd), vreinterpretq_u16_s16(y_coefficient));
			y_odd = vreinterpretq_s16_u16(vuzp2q_u16(vreinterpretq_u16_u32(ab3210), vreinterpretq_u16_u32(ab7654)));

			int16x8_t r_even = vqaddq_s16(rc, y_even);
			int16x8_t r_odd = vqaddq_s16(rc, y_odd);
			int16x8_t g_even = vqaddq_s16(gc, y_even);
			int16x8_t g_odd = vqaddq_s16(gc, y_odd);
			int16x8_t b_even = vqaddq_s16(bc, y_even);
			int16x8_t b_odd = vqaddq_s16(bc, y_odd);

			/* round */
			r_even = vshrq_n_s16(vaddq_s16(r_even, round_1bit), 1);
			r_odd  = vshrq_n_s16(vaddq_s16(r_odd, round_1bit), 1);
			g_even = vshrq_n_s16(vaddq_s16(g_even, round_1bit), 1);
			g_odd  = vshrq_n_s16(vaddq_s16(g_odd, round_1bit), 1);
			b_even = vshrq_n_s16(vaddq_s16(b_even, round_1bit), 1);
			b_odd  = vshrq_n_s16(vaddq_s16(b_odd, round_1bit), 1);

			/* combine even and odd bytes in original order */
			uint8x16_t r = vcombine_u8(vqmovun_s16(r_even), vqmovun_s16(r_odd));
			uint8x16_t g = vcombine_u8(vqmovun_s16(g_even), vqmovun_s16(g_odd));
			uint8x16_t b = vcombine_u8(vqmovun_s16(b_even), vqmovun_s16(b_odd));

			r = vzip1q_u8(r, vreinterpretq_u8_u64(vdupq_laneq_u64(vreinterpretq_u64_u8(r), 1)));
			g = vzip1q_u8(g, vreinterpretq_u8_u64(vdupq_laneq_u64(vreinterpretq_u64_u8(g), 1)));
			b = vzip1q_u8(b, vreinterpretq_u8_u64(vdupq_laneq_u64(vreinterpretq_u64_u8(b), 1)));

			/* Create RGBA (we could generate A here, but we don't) quads */
			uint8x16_t rg_l    = vzip1q_u8(r, g);
			uint8x16_t ba_l    = vzip1q_u8(b, alpha);
			uint16x8_t rgba_ll = vzip1q_u16(vreinterpretq_u16_u8(rg_l), vreinterpretq_u16_u8(ba_l));
			uint16x8_t rgba_lh = vzip2q_u16(vreinterpretq_u16_u8(rg_l), vreinterpretq_u16_u8(ba_l));

			uint8x16_t rg_h = vzip2q_u8(r, g);
			uint8x16_t ba_h = vzip2q_u8(b, alpha);
			uint16x8_t rgba_hl = vzip1q_u16(vreinterpretq_u16_u8(rg_h), vreinterpretq_u16_u8(ba_h));
			uint16x8_t rgba_hh = vzip2q_u16(vreinterpretq_u16_u8(rg_h), vreinterpretq_u16_u8(ba_h));

			vst1q_u8(reinterpret_cast<u8*>(&decoder.rgb32.c[n * 2 + m][0]), vreinterpretq_u8_u16(rgba_ll));
			vst1q_u8(reinterpret_cast<u8*>(&decoder.rgb32.c[n * 2 + m][4]), vreinterpretq_u8_u16(rgba_lh));
			vst1q_u8(reinterpret_cast<u8*>(&decoder.rgb32.c[n * 2 + m][8]), vreinterpretq_u8_u16(rgba_hl));
			vst1q_u8(reinterpret_cast<u8*>(&decoder.rgb32.c[n * 2 + m][12]), vreinterpretq_u8_u16(rgba_hh));
		}
	}
}
#else /* Reference C implementation */
	const macroblock_8& mb8 = decoder.mb8;
	macroblock_rgb32& rgb32 = decoder.rgb32;

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			s32 lum = (IPU_Y_COEFF * (std::max(0, (s32)mb8.Y[y][x] - IPU_Y_BIAS))) >> 6;
			s32 rcr = (IPU_RCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcr = (IPU_GCR_COEFF * ((s32)mb8.Cr[y>>1][x>>1] - 128)) >> 6;
			s32 gcb = (IPU_GCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;
			s32 bcb = (IPU_BCB_COEFF * ((s32)mb8.Cb[y>>1][x>>1] - 128)) >> 6;

			rgb32.c[y][x].r = std::max(0, std::min(255, (lum + rcr + 1) >> 1));
			rgb32.c[y][x].g = std::max(0, std::min(255, (lum + gcr + gcb + 1) >> 1));
			rgb32.c[y][x].b = std::max(0, std::min(255, (lum + bcb + 1) >> 1));
			rgb32.c[y][x].a = 0x80; /* the norm to save doing this on the alpha pass */
		}
#endif
}

static void ipu_csc(macroblock_8& mb8, macroblock_rgb32& rgb32, int sgn);
static void ipu_vq(macroblock_rgb16& rgb16, u8* indx4);

// --------------------------------------------------------------------------------------
//  Buffer reader
// --------------------------------------------------------------------------------------

__ri static u32 UBITS(uint bits)
{
	uint readpos8 = g_BP.BP/8;

	uint result = BigEndian(*(u32*)( (u8*)g_BP.internal_qwc + readpos8 ));
	uint bp7 = (g_BP.BP & 7);
	result <<= bp7;
	result >>= (32 - bits);

	return result;
}

__ri static s32 SBITS(uint bits)
{
	// Read an unaligned 32 bit value and then shift the bits up and then back down.

	uint readpos8 = g_BP.BP/8;

	int result = BigEndian(*(s32*)( (s8*)g_BP.internal_qwc + readpos8 ));
	uint bp7 = (g_BP.BP & 7);
	result <<= bp7;
	result >>= (32 - bits);

	return result;
}

#define GETWORD() g_BP.FillBuffer(16)

// Removes bits from the bitstream.  This is done independently of UBITS/SBITS because a
// lot of mpeg streams have to read ahead and rewind bits and re-read them at different
// bit depths or sign'age.
#define DUMPBITS(num) g_BP.Advance(num)

__fi static u32 GETBITS(uint num)
{
	uint ret = UBITS(num);
	g_BP.Advance(num);
	return ret;
}

// whenever reading fractions of bytes. The low bits always come from the next byte
// while the high bits come from the current byte
__ri static u8 getBits64(u8 *address)
{
	if (!g_BP.FillBuffer(64)) return 0;

	const u8* readpos = &g_BP.internal_qwc[0]._u8[g_BP.BP/8];

	if (uint shift = (g_BP.BP & 7))
	{
		u64 mask = (0xff >> shift);
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24) | (mask << 32) | (mask << 40) | (mask << 48) | (mask << 56);

		*(u64*)address = ((~mask & *(u64*)(readpos + 1)) >> (8 - shift)) | (((mask) & *(u64*)readpos) << shift);
	}
	else
		*(u64*)address = *(u64*)readpos;

	g_BP.Advance(64);

	return 1;
}

// whenever reading fractions of bytes. The low bits always come from the next byte
// while the high bits come from the current byte
__ri static u8 getBits32(u8 *address)
{
	if (!g_BP.FillBuffer(32)) return 0;

	const u8* readpos = &g_BP.internal_qwc->_u8[g_BP.BP/8];

	if(uint shift = (g_BP.BP & 7))
	{
		u32 mask = (0xff >> shift);
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24);

		*(u32*)address = ((~mask & *(u32*)(readpos + 1)) >> (8 - shift)) | (((mask) & *(u32*)readpos) << shift);
	}
	else
	{
		// Bit position-aligned -- no masking/shifting necessary
		*(u32*)address = *(u32*)readpos;
	}

	return 1;
}

__ri static u8 getBits8(u8 *address)
{
	if (!g_BP.FillBuffer(8)) return 0;

	const u8* readpos     = &g_BP.internal_qwc[0]._u8[g_BP.BP/8];

	if (uint shift = (g_BP.BP & 7))
	{
		uint mask     = (0xff >> shift);
		*(u8*)address = (((~mask) & readpos[1]) >> (8 - shift)) | (((mask) & *readpos) << shift);
	}
	else
		*(u8*)address = *(u8*)readpos;

	return 1;
}


#define W1 2841 /* 2048*sqrt (2)*cos (1*pi/16) */
#define W2 2676 /* 2048*sqrt (2)*cos (2*pi/16) */
#define W3 2408 /* 2048*sqrt (2)*cos (3*pi/16) */
#define W5 1609 /* 2048*sqrt (2)*cos (5*pi/16) */
#define W6 1108 /* 2048*sqrt (2)*cos (6*pi/16) */
#define W7 565  /* 2048*sqrt (2)*cos (7*pi/16) */

/*
 * In legal streams, the IDCT output should be between -384 and +384.
 * In corrupted streams, it is possible to force the IDCT output to go
 * to +-3826 - this is the worst case for a column IDCT where the
 * column inputs are 16-bit values.
 */

__fi static void BUTTERFLY(int& t0, int& t1, int w0, int w1, int d0, int d1)
{
	int tmp = w0 * (d0 + d1);
	t0 = tmp + (w1 - w0) * d1;
	t1 = tmp - (w1 + w0) * d0;
}

__ri static void IDCT_Block(s16* block)
{
	for (int i = 0; i < 8; i++)
	{
		s16* const rblock = block + 8 * i;
		if (!(rblock[1] | ((s32*)rblock)[1] | ((s32*)rblock)[2] |
				((s32*)rblock)[3]))
		{
			u32 tmp = (u16)(rblock[0] << 3);
			tmp |= tmp << 16;
			((s32*)rblock)[0] = tmp;
			((s32*)rblock)[1] = tmp;
			((s32*)rblock)[2] = tmp;
			((s32*)rblock)[3] = tmp;
			continue;
		}

		int a0, a1, a2, a3;
		{
			const int d0 = (rblock[0] << 11) + 128;
			const int d1 = rblock[1];
			const int d2 = rblock[2] << 11;
			const int d3 = rblock[3];
			int t0 = d0 + d2;
			int t1 = d0 - d2;
			int t2, t3;
			BUTTERFLY(t2, t3, W6, W2, d3, d1);
			a0 = t0 + t2;
			a1 = t1 + t3;
			a2 = t1 - t3;
			a3 = t0 - t2;
		}

		int b0, b1, b2, b3;
		{
			const int d0 = rblock[4];
			const int d1 = rblock[5];
			const int d2 = rblock[6];
			const int d3 = rblock[7];
			int t0, t1, t2, t3;
			BUTTERFLY(t0, t1, W7, W1, d3, d0);
			BUTTERFLY(t2, t3, W3, W5, d1, d2);
			b0 = t0 + t2;
			b3 = t1 + t3;
			t0 -= t2;
			t1 -= t3;
			b1 = ((t0 + t1) * 181) >> 8;
			b2 = ((t0 - t1) * 181) >> 8;
		}

		rblock[0] = (a0 + b0) >> 8;
		rblock[1] = (a1 + b1) >> 8;
		rblock[2] = (a2 + b2) >> 8;
		rblock[3] = (a3 + b3) >> 8;
		rblock[4] = (a3 - b3) >> 8;
		rblock[5] = (a2 - b2) >> 8;
		rblock[6] = (a1 - b1) >> 8;
		rblock[7] = (a0 - b0) >> 8;
	}

	for (int i = 0; i < 8; i++)
	{
		s16* const cblock = block + i;

		int a0, a1, a2, a3;
		{
			const int d0 = (cblock[8 * 0] << 11) + 65536;
			const int d1 = cblock[8 * 1];
			const int d2 = cblock[8 * 2] << 11;
			const int d3 = cblock[8 * 3];
			const int t0 = d0 + d2;
			const int t1 = d0 - d2;
			int t2;
			int t3;
			BUTTERFLY(t2, t3, W6, W2, d3, d1);
			a0 = t0 + t2;
			a1 = t1 + t3;
			a2 = t1 - t3;
			a3 = t0 - t2;
		}

		int b0, b1, b2, b3;
		{
			const int d0 = cblock[8 * 4];
			const int d1 = cblock[8 * 5];
			const int d2 = cblock[8 * 6];
			const int d3 = cblock[8 * 7];
			int t0, t1, t2, t3;
			BUTTERFLY(t0, t1, W7, W1, d3, d0);
			BUTTERFLY(t2, t3, W3, W5, d1, d2);
			b0 = t0 + t2;
			b3 = t1 + t3;
			t0 = (t0 - t2) >> 8;
			t1 = (t1 - t3) >> 8;
			b1 = (t0 + t1) * 181;
			b2 = (t0 - t1) * 181;
		}

		cblock[8 * 0] = (a0 + b0) >> 17;
		cblock[8 * 1] = (a1 + b1) >> 17;
		cblock[8 * 2] = (a2 + b2) >> 17;
		cblock[8 * 3] = (a3 + b3) >> 17;
		cblock[8 * 4] = (a3 - b3) >> 17;
		cblock[8 * 5] = (a2 - b2) >> 17;
		cblock[8 * 6] = (a1 - b1) >> 17;
		cblock[8 * 7] = (a0 - b0) >> 17;
	}
}

__ri static void IDCT_Copy(s16* block, u8* dest, const int stride)
{
	IDCT_Block(block);

	for (int i = 0; i < 8; i++)
	{
		dest[0] = (g_idct_clip_lut.data() + 384)[block[0]];
		dest[1] = (g_idct_clip_lut.data() + 384)[block[1]];
		dest[2] = (g_idct_clip_lut.data() + 384)[block[2]];
		dest[3] = (g_idct_clip_lut.data() + 384)[block[3]];
		dest[4] = (g_idct_clip_lut.data() + 384)[block[4]];
		dest[5] = (g_idct_clip_lut.data() + 384)[block[5]];
		dest[6] = (g_idct_clip_lut.data() + 384)[block[6]];
		dest[7] = (g_idct_clip_lut.data() + 384)[block[7]];

		memset(block, 0, 16);

		dest += stride;
		block += 8;
	}
}


// stride = increment for dest in 16-bit units (typically either 8 [128 bits] or 16 [256 bits]).
__ri static void IDCT_Add(const int last, s16* block, s16* dest, const int stride)
{
	// on the IPU, stride is always assured to be multiples of QWC (bottom 3 bits are 0).

	if (last != 129 || (block[0] & 7) == 4)
	{
		IDCT_Block(block);

		const r128 zero = r128_zero();
		for (int i = 0; i < 8; i++)
		{
			r128_store(dest, r128_load(block));
			r128_store(block, zero);

			dest += stride;
			block += 8;
		}
	}
	else
	{
		const u16 DC     = static_cast<u16>((static_cast<s32>(block[0]) + 4) >> 3);
		const r128 dc128 = r128_from_u32_dup(static_cast<u32>(DC) | (static_cast<u32>(DC) << 16));
		block[0] = block[63] = 0;

		for (int i = 0; i < 8; ++i)
			r128_store((dest + (stride * i)), dc128);
	}
}

/* Bitstream and buffer needs to be reallocated in order for successful
   reading of the old data. Here the old data stored in the 2nd slot
   of the internal buffer is copied to 1st slot, and the new data read
   into 1st slot is copied to the 2nd slot. Which will later be copied
   back to the 1st slot when 128bits have been read.
*/
static const DCTtab * tab;
static int mbaCount = 0;

static int GetMacroblockModes(void)
{
	int macroblock_modes;
	const MBtab * tab;

	switch (decoder.coding_type)
	{
		case I_TYPE:
			macroblock_modes = UBITS(2);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_I + (macroblock_modes >> 1);
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if ((!(decoder.frame_pred_frame_dct)) &&
				(decoder.picture_structure == FRAME_PICTURE))
				macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;
			return macroblock_modes;

		case P_TYPE:
			macroblock_modes = UBITS(6);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_P + (macroblock_modes >> 1);
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if (decoder.picture_structure != FRAME_PICTURE)
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				return macroblock_modes;
			}
			else if (decoder.frame_pred_frame_dct)
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= MC_FRAME;

				return macroblock_modes;
			}
			else
			{
				if (macroblock_modes & MACROBLOCK_MOTION_FORWARD)
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
					macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;

				return macroblock_modes;
			}

		case B_TYPE:
			macroblock_modes = UBITS(6);

			if (macroblock_modes == 0) return 0;   // error

			tab = MB_B + macroblock_modes;
			DUMPBITS(tab->len);
			macroblock_modes = tab->modes;

			if (decoder.picture_structure != FRAME_PICTURE)
			{
				if (!(macroblock_modes & MACROBLOCK_INTRA))
					macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;
				return (macroblock_modes | (tab->len << 16));
			}
			else if (decoder.frame_pred_frame_dct)
			{
				/* if (! (macroblock_modes & MACROBLOCK_INTRA)) */
				macroblock_modes |= MC_FRAME;
				return (macroblock_modes | (tab->len << 16));
			}
			else
			{
				if (macroblock_modes & MACROBLOCK_INTRA) goto intra;

				macroblock_modes |= GETBITS(2) * MOTION_TYPE_BASE;

				if (macroblock_modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
				{
intra:
					macroblock_modes |= GETBITS(1) * DCT_TYPE_INTERLACED;
				}
				return (macroblock_modes | (tab->len << 16));
			}

		case D_TYPE:
			macroblock_modes = GETBITS(1);
			//I suspect (as this is actually a 2 bit command) that this should be getbits(2)
			//additionally, we arent dumping any bits here when i think we should be, need a game to test. (Refraction)
			if (macroblock_modes != 0)
				return (MACROBLOCK_INTRA | (1 << 16));
			break;
		default:
			break;
	}
	return 0;
}

__ri static int get_macroblock_address_increment(void)
{
	const MBAtab *mba;
	u16 code = UBITS(16);

	if (code >= 4096)
		mba = MBA.mba5 + (UBITS(5) - 2);
	else if (code >= 768)
		mba = MBA.mba11 + (UBITS(11) - 24);
	else switch (UBITS(11))
	{
		case 8:		/* macroblock_escape */
			DUMPBITS(11);
			return 0xb0023;

		case 15:	/* macroblock_stuffing (MPEG1 only) */
			if (decoder.mpeg1)
			{
				DUMPBITS(11);
				return 0xb0022;
			}
			/* fall-through */

		default:
			return 0;//error
	}

	DUMPBITS(mba->len);

	return ((mba->mba + 1) | (mba->len << 16));
}

__fi static int get_luma_dc_dct_diff()
{
	int size;
	u16 code    = UBITS(5);

	if (code < 31)
	{
		size = DCtable.lum0[code].size;
		DUMPBITS(DCtable.lum0[code].len);

		// 5 bits max
	}
	else
	{
		code = UBITS(9) - 0x1f0;
		size = DCtable.lum1[code].size;
		DUMPBITS(DCtable.lum1[code].len);

		// 9 bits max
	}

	if (size != 0)
	{
		int dc_diff = GETBITS(size);

		// 6 for tab0 and 11 for tab1
		if ((dc_diff & (1<<(size-1)))==0)
		  dc_diff-= (1<<size) - 1;
		return dc_diff;
	}

	return 0;
}

__fi static int get_chroma_dc_dct_diff(void)
{
	int size;
	u16 code    = UBITS(5);

	if (code < 31)
	{
		size = DCtable.chrom0[code].size;
		DUMPBITS(DCtable.chrom0[code].len);
	}
	else
	{
		code = UBITS(10) - 0x3e0;
		size = DCtable.chrom1[code].size;
		DUMPBITS(DCtable.chrom1[code].len);
	}

	if (size != 0)
	{
		int dc_diff = GETBITS(size);

		if ((dc_diff & (1<<(size-1)))==0)
			dc_diff -= (1<<size) - 1;
		return dc_diff;
	}

	return 0;
}

__ri static bool get_intra_block(void)
{
	const u8 * scan = decoder.scantype ? mpeg2_scan.alt : mpeg2_scan.norm;
	const u8 (&quant_matrix)[64] = decoder.iq;
	int quantizer_scale = decoder.quantizer_scale;
	s16 * dest = decoder.DCTblock;
	u16 code;

	/* decode AC coefficients */
	for (int i=1 + ipu_cmd.pos[4]; ; i++)
	{
		switch (ipu_cmd.pos[5])
		{
			case 0:
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i - 1;
					return false;
				}

				code = UBITS(16);

				if (code >= 16384 && (!decoder.intra_vlc_format || decoder.mpeg1))
					tab = &DCT.next[(code >> 12) - 4];
				else if (code >= 1024)
				{
					if (decoder.intra_vlc_format && !decoder.mpeg1)
						tab = &DCT.tab0a[(code >> 8) - 4];
					else
						tab = &DCT.tab0[(code >> 8) - 4];
				}
				else if (code >= 512)
				{
					if (decoder.intra_vlc_format && !decoder.mpeg1)
						tab = &DCT.tab1a[(code >> 6) - 8];
					else
						tab = &DCT.tab1[(code >> 6) - 8];
				}

				// [TODO] Optimization: Following codes can all be done by a single "expedited" lookup
				// that should use a single unrolled DCT table instead of five separate tables used
				// here.  Multiple conditional statements are very slow, while modern CPU data caches
				// have lots of room to spare.

				else if (code >= 256)
					tab = &DCT.tab2[(code >> 4) - 16];
				else if (code >= 128)
					tab = &DCT.tab3[(code >> 3) - 16];
				else if (code >= 64)
					tab = &DCT.tab4[(code >> 2) - 16];
				else if (code >= 32)
					tab = &DCT.tab5[(code >> 1) - 16];
				else if (code >= 16)
					tab = &DCT.tab6[code - 16];
				else
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				DUMPBITS(tab->len);

				if (tab->run==64) /* end_of_block */
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				i += (tab->run == 65) ? GETBITS(6) : tab->run;
				if (i >= 64)
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}
				/* fall-through */

			case 1:
				{
					if (!GETWORD())
					{
						ipu_cmd.pos[4] = i - 1;
						ipu_cmd.pos[5] = 1;
						return false;
					}

					uint j = scan[i];
					int val;

					if (tab->run==65) /* escape */
					{
						if(!decoder.mpeg1)
						{
							val = (SBITS(12) * quantizer_scale * quant_matrix[i]) >> 4;
							DUMPBITS(12);
						}
						else
						{
							val = SBITS(8);
							DUMPBITS(8);

							if (!(val & 0x7f))
								val = GETBITS(8) + 2 * val;

							val = (val * quantizer_scale * quant_matrix[i]) >> 4;
							val = (val + ~ (((s32)val) >> 31)) | 1;
						}
					}
					else
					{
						val = (tab->level * quantizer_scale * quant_matrix[i]) >> 4;
						if(decoder.mpeg1) /* oddification */
							val = (val - 1) | 1;

						/* if (bitstream_get (1)) val = -val; */
						int bit1 = SBITS(1);
						val      = (val ^ bit1) - bit1;
						DUMPBITS(1);
					}


					if ((u32)(val + 2048) > 4095)
						val = (val >> 31) ^ 2047;
					dest[j] = val;
					ipu_cmd.pos[5] = 0;
				}
		}
	}

	ipu_cmd.pos[4] = 0;
	return true;
}

__ri static bool get_non_intra_block(int * last)
{
	int i;
	int j;
	int val;
	const u8 * scan = decoder.scantype ? mpeg2_scan.alt : mpeg2_scan.norm;
	const u8 (&quant_matrix)[64] = decoder.niq;
	int quantizer_scale = decoder.quantizer_scale;
	s16 * dest = decoder.DCTblock;
	u16 code;

	/* decode AC coefficients */
	for (i= ipu_cmd.pos[4] ; ; i++)
	{
		switch (ipu_cmd.pos[5])
		{
			case 0:
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i;
					return false;
				}

				code = UBITS(16);

				if (code >= 16384)
				{
					if (i==0)
						tab = &DCT.first[(code >> 12) - 4];
					else
						tab = &DCT.next[(code >> 12)- 4];
				}
				else if (code >= 1024)
					tab = &DCT.tab0[(code >> 8) - 4];
				else if (code >= 512)
					tab = &DCT.tab1[(code >> 6) - 8];

				// [TODO] Optimization: Following codes can all be done by a single "expedited" lookup
				// that should use a single unrolled DCT table instead of five separate tables used
				// here.  Multiple conditional statements are very slow, while modern CPU data caches
				// have lots of room to spare.

				else if (code >= 256)
					tab = &DCT.tab2[(code >> 4) - 16];
				else if (code >= 128)
					tab = &DCT.tab3[(code >> 3) - 16];
				else if (code >= 64)
					tab = &DCT.tab4[(code >> 2) - 16];
				else if (code >= 32)
					tab = &DCT.tab5[(code >> 1) - 16];
				else if (code >= 16)
					tab = &DCT.tab6[code - 16];
				else
				{
					ipu_cmd.pos[4] = 0;
					return true;
				}

				DUMPBITS(tab->len);

				if (tab->run==64) /* end_of_block */
				{
					*last = i;
					ipu_cmd.pos[4] = 0;
					return true;
				}

				i += (tab->run == 65) ? GETBITS(6) : tab->run;
				if (i >= 64)
				{
					*last = i;
					ipu_cmd.pos[4] = 0;
					return true;
				}
				/* fall-through */

			case 1:
				if (!GETWORD())
				{
					ipu_cmd.pos[4] = i;
					ipu_cmd.pos[5] = 1;
					return false;
				}

				j = scan[i];

				if (tab->run==65) /* escape */
				{
					if (!decoder.mpeg1)
					{
						val = ((2 * (SBITS(12) + SBITS(1)) + 1) * quantizer_scale * quant_matrix[i]) >> 5;
						DUMPBITS(12);
					}
					else
					{
						val = SBITS(8);
						DUMPBITS(8);

						if (!(val & 0x7f))
							val = GETBITS(8) + 2 * val;

						val = ((2 * (val + (((s32)val) >> 31)) + 1) * quantizer_scale * quant_matrix[i]) / 32;
						val = (val + ~ (((s32)val) >> 31)) | 1;
					}
				}
				else
				{
					int bit1 = SBITS(1);
					val = ((2 * tab->level + 1) * quantizer_scale * quant_matrix[i]) >> 5;
					val = (val ^ bit1) - bit1;
					DUMPBITS(1);
				}

				if ((u32)(val + 2048) > 4095)
					val = (val >> 31) ^ 2047;
				dest[j] = val;
				ipu_cmd.pos[5] = 0;
		}
	}

	ipu_cmd.pos[4] = 0;
	return true;
}

__ri static bool slice_intra_DCT(const int cc, u8 * const dest, const int stride, const bool skip)
{
	if (!skip || ipu_cmd.pos[3])
	{
		ipu_cmd.pos[3] = 0;
		if (!GETWORD())
		{
			ipu_cmd.pos[3] = 1;
			return false;
		}

		/* Get the intra DC coefficient and inverse quantize it */
		if (cc == 0)
			decoder.dc_dct_pred[0] += get_luma_dc_dct_diff();
		else
			decoder.dc_dct_pred[cc] += get_chroma_dc_dct_diff();

		decoder.DCTblock[0] = decoder.dc_dct_pred[cc] << (3 - decoder.intra_dc_precision);
	}

	if (!get_intra_block())
		return false;

	IDCT_Copy(decoder.DCTblock, dest, stride);

	return true;
}

__ri static bool slice_non_intra_DCT(s16 * const dest, const int stride, const bool skip)
{
	int last = 0;

	if (!skip)
		memset(decoder.DCTblock, 0, sizeof(decoder.DCTblock));

	if (!get_non_intra_block(&last))
		return false;

	IDCT_Add(last, decoder.DCTblock, dest, stride);

	return true;
}

#define finishmpeg2sliceIDEC() \
	ipuRegs.ctrl.SCD = 0; \
	coded_block_pattern = decoder.coded_block_pattern

__ri static bool mpeg2sliceIDEC(void)
{
	u16 code;

	static bool ready_to_decode = true;
	switch (ipu_cmd.pos[0])
	{
		case 0:
			decoder.dc_dct_pred[0] =
				decoder.dc_dct_pred[1] =
				decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;

			ipuRegs.top = 0;
			ipuRegs.ctrl.ECD = 0;
			/* fall-through */

		case 1:
			ipu_cmd.pos[0] = 1;
			if (!g_BP.FillBuffer(32))
				return false;
			/* fall-through */

		case 2:
			ipu_cmd.pos[0] = 2;
			for (;;)
			{
				// IPU0 isn't ready for data, so let's wait for it to be
				if ((!ipu0ch.chcr.STR || ipuRegs.ctrl.OFC || ipu0ch.qwc == 0) && ipu_cmd.pos[1] <= 2)
				{
					IPUCoreStatus.WaitingOnIPUFrom = true;
					return false;
				}
				macroblock_8& mb8 = decoder.mb8;
				macroblock_rgb16& rgb16 = decoder.rgb16;
				macroblock_rgb32& rgb32 = decoder.rgb32;

				int DCT_offset, DCT_stride;
				const MBAtab * mba;

				switch (ipu_cmd.pos[1])
				{
					case 0:
						decoder.macroblock_modes = GetMacroblockModes();

						if (decoder.macroblock_modes & MACROBLOCK_QUANT) //only IDEC
						{
							const int quantizer_scale_code = GETBITS(5);
							if (decoder.q_scale_type)
								decoder.quantizer_scale = non_linear_quantizer_scale[quantizer_scale_code];
							else
								decoder.quantizer_scale = quantizer_scale_code << 1;
						}

						decoder.coded_block_pattern = 0x3F;//all 6 blocks
						memset(&mb8, 0, sizeof(mb8));
						memset(&rgb32, 0, sizeof(rgb32));
						/* fall-through */

					case 1:
						ipu_cmd.pos[1] = 1;

						if (decoder.macroblock_modes & DCT_TYPE_INTERLACED)
						{
							DCT_offset = decoder_stride;
							DCT_stride = decoder_stride * 2;
						}
						else
						{
							DCT_offset = decoder_stride * 8;
							DCT_stride = decoder_stride;
						}

						switch (ipu_cmd.pos[2])
						{
							case 0:
							case 1:
								if (!slice_intra_DCT(0, (u8*)mb8.Y, DCT_stride, ipu_cmd.pos[2] == 1))
								{
									ipu_cmd.pos[2] = 1;
									return false;
								}
								/* fall-through */

							case 2:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + 8, DCT_stride, ipu_cmd.pos[2] == 2))
								{
									ipu_cmd.pos[2] = 2;
									return false;
								}
								/* fall-through */

							case 3:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset, DCT_stride, ipu_cmd.pos[2] == 3))
								{
									ipu_cmd.pos[2] = 3;
									return false;
								}
								/* fall-through */

							case 4:
								if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[2] == 4))
								{
									ipu_cmd.pos[2] = 4;
									return false;
								}
								/* fall-through */

							case 5:
								if (!slice_intra_DCT(1, (u8*)mb8.Cb, decoder_stride >> 1, ipu_cmd.pos[2] == 5))
								{
									ipu_cmd.pos[2] = 5;
									return false;
								}
								/* fall-through */

							case 6:
								if (!slice_intra_DCT(2, (u8*)mb8.Cr, decoder_stride >> 1, ipu_cmd.pos[2] == 6))
								{
									ipu_cmd.pos[2] = 6;
									return false;
								}
								break;
							default:
								break;
						}

						// Send The MacroBlock via DmaIpuFrom
						ipu_csc(mb8, rgb32, decoder.sgn);

						if (decoder.ofm == 0)
							decoder.SetOutputTo(rgb32);
						else
						{
							ipu_dither(rgb32, rgb16, decoder.dte);
							decoder.SetOutputTo(rgb16);
						}
						ipu_cmd.pos[1] = 2;

						/* fallthrough */

					case 2:
						{
							if (ready_to_decode == true)
							{
								ready_to_decode = false;
								IPUCoreStatus.WaitingOnIPUFrom = false;
								IPUCoreStatus.WaitingOnIPUTo = false;
								IPU_INT_PROCESS(64); // Should probably be much higher, but Myst 3 doesn't like it right now.
								ipu_cmd.pos[1] = 2;
								return false;
							}

							uint read = ipu_fifo.out.write((u32*)decoder.GetIpuDataPtr(), decoder.ipu0_data);
							decoder.AdvanceIpuDataBy(read);

							if (decoder.ipu0_data != 0)
							{
								// IPU FIFO filled up -- Will have to finish transferring later.
								IPUCoreStatus.WaitingOnIPUFrom = true;
								ipu_cmd.pos[1] = 2;
								return false;
							}

							mbaCount = 0;
							if (read)
							{
								IPUCoreStatus.WaitingOnIPUFrom = true;
								ipu_cmd.pos[1] = 3;
								return false;
							}
						}
						/* fall-through */

					case 3:
						ready_to_decode = true;
						for (;;)
						{
							if (!GETWORD())
							{
								ipu_cmd.pos[1] = 3;
								return false;
							}

							code = UBITS(16);
							if (code >= 0x1000)
							{
								mba = MBA.mba5 + (UBITS(5) - 2);
								break;
							}
							else if (code >= 0x0300)
							{
								mba = MBA.mba11 + (UBITS(11) - 24);
								break;
							}
							else switch (UBITS(11))
							{
								case 8:		/* macroblock_escape */
									mbaCount += 33;
									/* fall-through */

								case 15:	/* macroblock_stuffing (MPEG1 only) */
									DUMPBITS(11);
									continue;

								default:	/* end of slice/frame, or error? */
									goto finish_idec;
							}
						}

						DUMPBITS(mba->len);
						mbaCount += mba->mba;

						if (mbaCount)
							decoder.dc_dct_pred[0] =
							decoder.dc_dct_pred[1] =
							decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;
						/* fall-through */

					case 4:
						if (!GETWORD())
						{
							ipu_cmd.pos[1] = 4;
							return false;
						}
						break;
					default:
						break;
				}

				ipu_cmd.pos[1] = 0;
				ipu_cmd.pos[2] = 0;
			}

finish_idec:
			finishmpeg2sliceIDEC();
			/* fall-through */

		case 3:
			{
				u8 bit8;
				u32 start_check;
				if (!getBits8((u8*)&bit8))
				{
					ipu_cmd.pos[0] = 3;
					return false;
				}

				if (bit8 == 0)
				{
					g_BP.Align();
					for (;;)
					{
						if (!g_BP.FillBuffer(24))
						{
							ipu_cmd.pos[0] = 3;
							return false;
						}
						start_check = UBITS(24);
						if (start_check != 0)
						{
							if (start_check == 1)
								ipuRegs.ctrl.SCD = 1;
							else
								ipuRegs.ctrl.ECD = 1;
							break;
						}
						DUMPBITS(8);
					}
				}
			}
			/* fall-through */

		case 4:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 4;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);
			break;
		default:
			break;
	}

	return true;
}

__fi static bool mpeg2_slice(void)
{
	int DCT_offset, DCT_stride;
	static bool ready_to_decode = true;

	macroblock_8& mb8 = decoder.mb8;
	macroblock_16& mb16 = decoder.mb16;

	switch (ipu_cmd.pos[0])
	{
		case 0:
			if (decoder.dcr)
				decoder.dc_dct_pred[0] =
					decoder.dc_dct_pred[1] =
					decoder.dc_dct_pred[2] = 128 << decoder.intra_dc_precision;

			ipuRegs.ctrl.ECD = 0;
			ipuRegs.top = 0;
			memset(&mb8, 0, sizeof(mb8));
			memset(&mb16, 0, sizeof(mb16));
			/* fallthrough */

		case 1:
			if (!g_BP.FillBuffer(32))
			{
				ipu_cmd.pos[0] = 1;
				return false;
			}
			/* fall-through */

		case 2:
			ipu_cmd.pos[0] = 2;

			// IPU0 isn't ready for data, so let's wait for it to be
			if ((!ipu0ch.chcr.STR || ipuRegs.ctrl.OFC || ipu0ch.qwc == 0) && ipu_cmd.pos[0] <= 3)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}

			if (decoder.macroblock_modes & DCT_TYPE_INTERLACED)
			{
				DCT_offset = decoder_stride;
				DCT_stride = decoder_stride * 2;
			}
			else
			{
				DCT_offset = decoder_stride * 8;
				DCT_stride = decoder_stride;
			}

			if (decoder.macroblock_modes & MACROBLOCK_INTRA)
			{
				switch(ipu_cmd.pos[1])
				{
					case 0:
						decoder.coded_block_pattern = 0x3F;
						/* fall-through */

					case 1:
						if (!slice_intra_DCT(0, (u8*)mb8.Y, DCT_stride, ipu_cmd.pos[1] == 1))
						{
							ipu_cmd.pos[1] = 1;
							return false;
						}
						/* fall-through */

					case 2:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + 8, DCT_stride, ipu_cmd.pos[1] == 2))
						{
							ipu_cmd.pos[1] = 2;
							return false;
						}
						/* fall-through */

					case 3:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset, DCT_stride, ipu_cmd.pos[1] == 3))
						{
							ipu_cmd.pos[1] = 3;
							return false;
						}
						/* fall-through */

					case 4:
						if (!slice_intra_DCT(0, (u8*)mb8.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[1] == 4))
						{
							ipu_cmd.pos[1] = 4;
							return false;
						}
						/* fall-through */

					case 5:
						if (!slice_intra_DCT(1, (u8*)mb8.Cb, decoder_stride >> 1, ipu_cmd.pos[1] == 5))
						{
							ipu_cmd.pos[1] = 5;
							return false;
						}
						/* fall-through */

					case 6:
						if (!slice_intra_DCT(2, (u8*)mb8.Cr, decoder_stride >> 1, ipu_cmd.pos[1] == 6))
						{
							ipu_cmd.pos[1] = 6;
							return false;
						}
						break;
					default:
						break;
				}

				// Copy macroblock8 to macroblock16 - without sign extension.
				{
					uint i;
					const u8	*s = (const u8*)&mb8;
					u16		*d = (u16*)&mb16;
					//Y  bias	- 16 * 16
					//Cr bias	- 8 * 8
					//Cb bias	- 8 * 8
#if _M_SSE >= 0x200 /* SSE2 codepath */
					// Manually inlined due to MSVC refusing to inline the SSE-optimized version.
					__m128i zeroreg    = _mm_setzero_si128();

					for (uint i = 0; i < (256+64+64) / 32; ++i)
					{
						__m128i a1 = _mm_load_si128((__m128i*)s);
						__m128i a2 = _mm_load_si128((__m128i*)s+1);
						_mm_store_si128((__m128i*)d,	_mm_unpacklo_epi8(a1, zeroreg));
						_mm_store_si128((__m128i*)d+1,	_mm_unpackhi_epi8(a1, zeroreg));
						_mm_store_si128((__m128i*)d+2,	_mm_unpacklo_epi8(a2, zeroreg));
						_mm_store_si128((__m128i*)d+3,	_mm_unpackhi_epi8(a2, zeroreg));
						s += 32;
						d += 32;
					}
#elif defined(_M_ARM64) /* ARM64 codepath */
				uint8x16_t zeroreg = vmovq_n_u8(0);

				for (uint i = 0; i < (256 + 64 + 64) / 32; ++i)
				{
					//*d++ = *s++;
					uint8x16_t woot1 = vld1q_u8((uint8_t*)s);
					uint8x16_t woot2 = vld1q_u8((uint8_t*)s + 16);
					vst1q_u8((uint8_t*)d, vzip1q_u8(woot1, zeroreg));
					vst1q_u8((uint8_t*)d + 16, vzip2q_u8(woot1, zeroreg));
					vst1q_u8((uint8_t*)d + 32, vzip1q_u8(woot2, zeroreg));
					vst1q_u8((uint8_t*)d + 48, vzip2q_u8(woot2, zeroreg));
					s += 32;
					d += 32;
				}
#else
					/* Reference C version */
					for (i = 0; i < 256; i++) *d++ = *s++; /* Y  */
					for (i = 0; i < 64; i++)  *d++ = *s++; /* Cr */
					for (i = 0; i < 64; i++)  *d++ = *s++; /* Cb */
#endif
				}
			}
			else
			{
				if (decoder.macroblock_modes & MACROBLOCK_PATTERN)
				{
					switch (ipu_cmd.pos[1])
					{
						case 0:
							{
								// Get coded block pattern
								const CBPtab* tab;
								u16 code = UBITS(16);

								if (code >= 0x2000)
									tab = CBP_7 + (UBITS(7) - 16);
								else
									tab = CBP_9 + UBITS(9);

								DUMPBITS(tab->len);
								decoder.coded_block_pattern = tab->cbp;
							}
							/* fall-through */

						case 1:
							if (decoder.coded_block_pattern & 0x20)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y, DCT_stride, ipu_cmd.pos[1] == 1))
								{
									ipu_cmd.pos[1] = 1;
									return false;
								}
							}
							/* fall-through */

						case 2:
							if (decoder.coded_block_pattern & 0x10)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + 8, DCT_stride, ipu_cmd.pos[1] == 2))
								{
									ipu_cmd.pos[1] = 2;
									return false;
								}
							}
							/* fall-through */

						case 3:
							if (decoder.coded_block_pattern & 0x08)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + DCT_offset, DCT_stride, ipu_cmd.pos[1] == 3))
								{
									ipu_cmd.pos[1] = 3;
									return false;
								}
							}
							/* fall-through */

						case 4:
							if (decoder.coded_block_pattern & 0x04)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Y + DCT_offset + 8, DCT_stride, ipu_cmd.pos[1] == 4))
								{
									ipu_cmd.pos[1] = 4;
									return false;
								}
							}
							/* fall-through */

						case 5:
							if (decoder.coded_block_pattern & 0x2)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Cb, decoder_stride >> 1, ipu_cmd.pos[1] == 5))
								{
									ipu_cmd.pos[1] = 5;
									return false;
								}
							}
							/* fall-through */

						case 6:
							if (decoder.coded_block_pattern & 0x1)
							{
								if (!slice_non_intra_DCT((s16*)mb16.Cr, decoder_stride >> 1, ipu_cmd.pos[1] == 6))
								{
									ipu_cmd.pos[1] = 6;
									return false;
								}
							}
							break;
						default:
							break;
					}
				}
			}

			// Send The MacroBlock via DmaIpuFrom
			ipuRegs.ctrl.SCD = 0;
			coded_block_pattern = decoder.coded_block_pattern;

			decoder.SetOutputTo(mb16);

			/* fallthrough */

		case 3:
			{
				if (ready_to_decode == true)
				{
					ipu_cmd.pos[0]  = 3;
					ready_to_decode = false;
					IPUCoreStatus.WaitingOnIPUFrom = false;
					IPUCoreStatus.WaitingOnIPUTo = false;
					IPU_INT_PROCESS(64); // Should probably be much higher, but Myst 3 doesn't like it right now.
					return false;
				}

				uint read = ipu_fifo.out.write((u32*)decoder.GetIpuDataPtr(), decoder.ipu0_data);
				decoder.AdvanceIpuDataBy(read);

				if (decoder.ipu0_data != 0)
				{
					// IPU FIFO filled up -- Will have to finish transferring later.
					IPUCoreStatus.WaitingOnIPUFrom = true;
					ipu_cmd.pos[0] = 3;
					return false;
				}

				mbaCount = 0;
				if (read)
				{
					IPUCoreStatus.WaitingOnIPUFrom = true;
					ipu_cmd.pos[0] = 4;
					return false;
				}
			}
			/* fall-through */

		case 4:
			{
				u8 bit8;
				u32 start_check;
				if (!getBits8((u8*)&bit8))
				{
					ipu_cmd.pos[0] = 4;
					return false;
				}

				if (bit8 == 0)
				{
					g_BP.Align();

					for (;;)
					{
						if (!g_BP.FillBuffer(24))
						{
							ipu_cmd.pos[0] = 4;
							return false;
						}
						start_check = UBITS(24);
						if (start_check != 0)
						{
							if (start_check == 1)
								ipuRegs.ctrl.SCD = 1;
							else
								ipuRegs.ctrl.ECD = 1;
							break;
						}
						DUMPBITS(8);
					}
				}
			}
			/* fall-through */

		case 5:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 5;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);
			break;
	}

	ready_to_decode = true;
	return true;
}

//////////////////////////////////////////////////////
// IPU Commands (exec on worker thread only)

__fi static bool ipuVDEC(u32 val)
{
	static int count = 0;
	if (count++ > 5)
	{
		if (!FMVstarted)
		{
			EnableFMV = true;
			FMVstarted = true;
		}
		count = 0;
	}
	eecount_on_last_vdec = cpuRegs.cycle;

	switch (ipu_cmd.pos[0])
	{
		case 0:
			if (!g_BP.FillBuffer(32))
				return false;

			switch ((val >> 26) & 3)
			{
				case 0://Macroblock Address Increment
					decoder.mpeg1 = ipuRegs.ctrl.MP1;
					ipuRegs.cmd.DATA = get_macroblock_address_increment();
					break;

				case 1://Macroblock Type
					decoder.frame_pred_frame_dct = 1;
					decoder.coding_type = ipuRegs.ctrl.PCT > 0 ? ipuRegs.ctrl.PCT : 1; // Kaiketsu Zorro Mezase doesn't set a Picture type, seems happy with I
					ipuRegs.cmd.DATA = GetMacroblockModes();
					break;

				case 2://Motion Code
					{
						const u16 code = UBITS(16);
						if ((code & 0x8000))
						{
							DUMPBITS(1);
							ipuRegs.cmd.DATA = 0x00010000;
						}
						else
						{
							const MVtab* tab;
							if ((code & 0xf000) || ((code & 0xfc00) == 0x0c00))
								tab = MV_4 + UBITS(4);
							else
								tab = MV_10 + UBITS(10);

							const int delta = tab->delta + 1;
							DUMPBITS(tab->len);

							const int sign = SBITS(1);
							DUMPBITS(1);

							ipuRegs.cmd.DATA = (((delta ^ sign) - sign) | (tab->len << 16));
						}
					}
					break;

				case 3://DMVector
					{
						const DMVtab* tab = DMV_2 + UBITS(2);
						DUMPBITS(tab->len);
						ipuRegs.cmd.DATA = (tab->dmv | (tab->len << 16));
					}
					break;
				default:
					break;
			}

			// HACK ATTACK!  This code OR's the MPEG decoder's bitstream position into the upper
			// 16 bits of DATA; which really doesn't make sense since (a) we already rewound the bits
			// back into the IPU internal buffer above, and (b) the IPU doesn't have an MPEG internal
			// 32-bit decoder buffer of its own anyway.  Furthermore, setting the upper 16 bits to
			// any value other than zero appears to work fine.  When set to zero, however, FMVs run
			// very choppy (basically only decoding/updating every 30th frame or so). So yeah,
			// someone with knowledge on the subject please feel free to explain this one. :) --air

			// The upper bits are the "length" of the decoded command, where the lower is the address.
			// This is due to differences with IPU and the MPEG standard. See get_macroblock_address_increment().

			ipuRegs.ctrl.ECD = (ipuRegs.cmd.DATA == 0);
			/* fall-through */

		case 1:
			if (!getBits32((u8*)&ipuRegs.top))
			{
				ipu_cmd.pos[0] = 1;
				return false;
			}

			ipuRegs.top = BigEndian(ipuRegs.top);

			return true;

		default:
			break;
	}

	return false;
}

__ri static bool ipuFDEC(u32 val)
{
	if (!getBits32((u8*)&ipuRegs.cmd.DATA)) return false;

	ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
	ipuRegs.top = ipuRegs.cmd.DATA;

	return true;
}

static bool ipuSETIQ(u32 val)
{
	if ((val >> 27) & 1)
	{
		u8 (&niq)[64] = decoder.niq;

		for(;ipu_cmd.pos[0] < 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)niq + 8 * ipu_cmd.pos[0])) return false;
		}
	}
	else
	{
		u8 (&iq)[64] = decoder.iq;

		for(;ipu_cmd.pos[0] < 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)iq + 8 * ipu_cmd.pos[0])) return false;
		}
	}

	return true;
}

static bool ipuSETVQ(u32 val)
{
	for(;ipu_cmd.pos[0] < 4; ipu_cmd.pos[0]++)
	{
		if (!getBits64(((u8*)g_ipu_vqclut) + 8 * ipu_cmd.pos[0])) return false;
	}

	return true;
}

// IPU Transfers are split into 8Qwords so we need to send ALL the data
__ri static bool ipuCSC(tIPU_CMD_CSC csc)
{
	for (;ipu_cmd.index < (int)csc.MBC; ipu_cmd.index++)
	{
		for(;ipu_cmd.pos[0] < 48; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)&decoder.mb8 + 8 * ipu_cmd.pos[0])) return false;
		}

		ipu_csc(decoder.mb8, decoder.rgb32, 0);

		if (csc.OFM)
		{
			ipu_dither(decoder.rgb32, decoder.rgb16, csc.DTE);
			ipu_cmd.pos[1] += ipu_fifo.out.write(((u32*) & decoder.rgb16) + 4 * ipu_cmd.pos[1], 32 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 32)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}
		else
		{
			ipu_cmd.pos[1] += ipu_fifo.out.write(((u32*) & decoder.rgb32) + 4 * ipu_cmd.pos[1], 64 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 64)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}

		ipu_cmd.pos[0] = 0;
		ipu_cmd.pos[1] = 0;
	}

	return true;
}

__ri static bool ipuPACK(tIPU_CMD_CSC csc)
{
	for (;ipu_cmd.index < (int)csc.MBC; ipu_cmd.index++)
	{
		for(;ipu_cmd.pos[0] < (int)sizeof(macroblock_rgb32) / 8; ipu_cmd.pos[0]++)
		{
			if (!getBits64((u8*)&decoder.rgb32 + 8 * ipu_cmd.pos[0])) return false;
		}

		ipu_dither(decoder.rgb32, decoder.rgb16, csc.DTE);

		if (csc.OFM)
		{
			ipu_cmd.pos[1] += ipu_fifo.out.write(((u32*) & decoder.rgb16) + 4 * ipu_cmd.pos[1], 32 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 32)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}
		else
		{
			ipu_vq(decoder.rgb16, g_ipu_indx4);
			ipu_cmd.pos[1] += ipu_fifo.out.write(((u32*)g_ipu_indx4) + 4 * ipu_cmd.pos[1], 8 - ipu_cmd.pos[1]);
			if (ipu_cmd.pos[1] < 8)
			{
				IPUCoreStatus.WaitingOnIPUFrom = true;
				return false;
			}
		}

		ipu_cmd.pos[0] = 0;
		ipu_cmd.pos[1] = 0;
	}

	return true;
}

// --------------------------------------------------------------------------------------
//  CORE Functions (referenced from MPEG library)
// --------------------------------------------------------------------------------------

__fi static void ipu_csc(macroblock_8& mb8, macroblock_rgb32& rgb32, int sgn)
{
	int i;
	u8* p = (u8*)&rgb32;

	yuv2rgb();

	if (g_ipu_thresh[0] > 0)
	{
		for (i = 0; i < 16*16; i++, p += 4)
		{
			if ((p[0] < g_ipu_thresh[0]) && (p[1] < g_ipu_thresh[0]) && (p[2] < g_ipu_thresh[0]))
				*(u32*)p = 0;
			else if ((p[0] < g_ipu_thresh[1]) && (p[1] < g_ipu_thresh[1]) && (p[2] < g_ipu_thresh[1]))
				p[3] = 0x40;
		}
	}
	else if (g_ipu_thresh[1] > 0)
	{
		for (i = 0; i < 16*16; i++, p += 4)
		{
			if ((p[0] < g_ipu_thresh[1]) && (p[1] < g_ipu_thresh[1]) && (p[2] < g_ipu_thresh[1]))
				p[3] = 0x40;
		}
	}
	if (sgn)
	{
		for (i = 0; i < 16*16; i++, p += 4)
			*(u32*)p ^= 0x808080;
	}
}

__fi static void ipu_vq(macroblock_rgb16& rgb16, u8* indx4)
{
	const auto closest_index = [&](int i, int j) {
		u8 index = 0;
		int min_distance = std::numeric_limits<int>::max();
		for (u8 k = 0; k < 16; ++k)
		{
			const int dr = rgb16.c[i][j].r - g_ipu_vqclut[k].r;
			const int dg = rgb16.c[i][j].g - g_ipu_vqclut[k].g;
			const int db = rgb16.c[i][j].b - g_ipu_vqclut[k].b;
			const int distance = dr * dr + dg * dg + db * db;

			// XXX: If two distances are the same which index is used?
			if (min_distance > distance)
			{
				index = k;
				min_distance = distance;
			}
		}

		return index;
	};

	for (int i = 0; i < 16; ++i)
		for (int j = 0; j < 8; ++j)
			indx4[i * 8 + j] = closest_index(i, 2 * j + 1) << 4 | closest_index(i, 2 * j);
}

__noinline void IPUWorker(void)
{
	switch (ipu_cmd.CMD)
	{
		// These are unreachable (BUSY will always be 0 for them)
		//case SCE_IPU_BCLR:
		//case SCE_IPU_SETTH:
			//break;

		case SCE_IPU_IDEC:
			if (!mpeg2sliceIDEC()) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_BDEC:
			if (!mpeg2_slice()) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;

			break;

		case SCE_IPU_VDEC:
			if (!ipuVDEC(ipu_cmd.current)) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_FDEC:
			if (!ipuFDEC(ipu_cmd.current)) return;

			ipuRegs.topbusy = 0;
			ipuRegs.cmd.BUSY = 0;
			break;

		case SCE_IPU_SETIQ:
			if (!ipuSETIQ(ipu_cmd.current)) return;
			break;

		case SCE_IPU_SETVQ:
			if (!ipuSETVQ(ipu_cmd.current)) return;
			break;

		case SCE_IPU_CSC:
			{
				tIPU_CMD_CSC _val;
				_val._u32 = ipu_cmd.current;
				if (!ipuCSC(_val))
					return;
			}
			break;

		case SCE_IPU_PACK:
			{
				tIPU_CMD_CSC _val;
				_val._u32 = ipu_cmd.current;
				if (!ipuPACK(_val))
					return;
			}
			break;
		default:
			break;
	}

	// success
	ipuRegs.ctrl.BUSY = 0;
	hwIntcIrq(INTC_IPU);
}

MULTI_ISA_UNSHARED_END

void ipuDmaReset(void)
{
	IPU1Status.InProgress	= false;
	IPU1Status.DMAFinished	= true;
}

bool SaveStateBase::ipuDmaFreeze()
{
	if (!(FreezeTag( "IPUdma" )))
		return false;

	Freeze(IPU1Status);

	return IsOkay();
}

static __fi int IPU1chain(void)
{
	u32 *pMem = (u32*)dmaGetAddr(ipu1ch.madr, false);

	if (pMem)
	{
		//Write our data to the FIFO
		int qwc      = ipu_fifo.in.write(pMem, ipu1ch.qwc);
		ipu1ch.madr += qwc << 4;
		ipu1ch.qwc  -= qwc;

		//Update TADR etc
		hwDmacSrcTadrInc(ipu1ch);

		if (!ipu1ch.qwc)
			IPU1Status.InProgress = false;
		return qwc;
	}

	return 0;
}

void IPU1dma(void)
{
	if(!ipu1ch.chcr.STR || ipu1ch.chcr.MOD == 2)
	{
		//We MUST stop the IPU from trying to fill the FIFO with more data if the DMA has been suspended
		//if we don't, we risk causing the data to go out of sync with the fifo and we end up losing some!
		//This is true for Dragons Quest 8 and probably others which suspend the DMA.
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);
		return;
	}

	if (!IPUCoreStatus.DataRequested)
	{
		// IPU isn't expecting any data, so put it in to wait mode.
		cpuRegs.eCycle[4] = 0x9999;
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);

		// Shouldn't Happen.
		if (IPUCoreStatus.WaitingOnIPUTo)
		{
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(4 * BIAS);
		}
		return;
	}

	int tagcycles = 0;
	int totalqwc = 0;

	if (!IPU1Status.InProgress)
	{
		tDMA_TAG* ptag = dmaGetAddr(ipu1ch.tadr, false);  //Set memory pointer to TADR

		if (!ipu1ch.transfer(ptag))
			return;
		ipu1ch.madr = ptag[1]._u32;

		tagcycles += 1; // Add 1 cycles from the QW read for the tag

		IPU1Status.DMAFinished = hwDmacSrcChain(ipu1ch, ptag->ID);

		if (ipu1ch.chcr.TIE && ptag->IRQ) //Tag Interrupt is set, so schedule the end/interrupt
			IPU1Status.DMAFinished = true;

		if (ipu1ch.qwc)
			IPU1Status.InProgress = true;
	}

	if (IPU1Status.InProgress)
		totalqwc += IPU1chain();

	// Nothing has been processed except maybe a tag, or the DMA is ending
	if(totalqwc == 0 || (IPU1Status.DMAFinished && !IPU1Status.InProgress))
	{
		totalqwc = std::max(4, totalqwc) + tagcycles;
		IPU_INT_TO(totalqwc * BIAS);
	}
	else
	{
		cpuRegs.eCycle[4] = 0x9999;
		CPU_SET_DMASTALL(DMAC_TO_IPU, true);
	}

	if (IPUCoreStatus.WaitingOnIPUTo && g_BP.IFC >= 1)
	{
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPU_INT_PROCESS(totalqwc * BIAS);
	}
}

void IPU0dma(void)
{
	if(!ipuRegs.ctrl.OFC)
	{
		/* This shouldn't happen. */
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUProcessInterrupt();
		}
		CPU_SET_DMASTALL(DMAC_FROM_IPU, true);
		return;
	}

	int readsize;
	tDMA_TAG* pMem;

	if ((!(ipu0ch.chcr.STR) || (cpuRegs.interrupt & (1 << DMAC_FROM_IPU))) || (ipu0ch.qwc == 0))
	{
		// This shouldn't happen.
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPU_INT_PROCESS(ipuRegs.ctrl.OFC * BIAS);
		}
		return;
	}

	pMem = dmaGetAddr(ipu0ch.madr, true);

	readsize = std::min(ipu0ch.qwc, (u32)ipuRegs.ctrl.OFC);
	ipu_fifo.out.read(pMem, readsize);

	ipu0ch.madr += readsize << 4;
	ipu0ch.qwc -= readsize;

	if (dmacRegs.ctrl.STS == STS_fromIPU)   // STS == fromIPU
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	if (!ipu0ch.qwc)
		IPU_INT_FROM(readsize * BIAS);

	CPU_SET_DMASTALL(DMAC_FROM_IPU, true);

	if (ipuRegs.ctrl.BUSY && IPUCoreStatus.WaitingOnIPUFrom)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPU_INT_PROCESS(readsize * BIAS);
	}
}

__fi void dmaIPU0(void) // fromIPU
{
	if (dmacRegs.ctrl.STS == STS_fromIPU)   // STS == fromIPU - Initial settings
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
	// Note: This should probably be a very small value, however anything lower than this will break Mana Khemia
	// This is because the game sends bad DMA information, starts an IDEC, then sets it to the correct values
	// but because our IPU is too quick, it messes up the sync between the DMA and IPU.
	// So this will do until (if) we sort the timing out of IPU, shouldn't cause any problems for games for now.
	//IPU_INT_FROM( 160 );
	// Update 22/12/2021 - Doesn't seem to need this now after fixing some FIFO/DMA behaviour
	IPU0dma();

	// Explanation of this:
	// The DMA logic on a NORMAL transfer is generally a "transfer first, ask questions later" so when it's sent
	// QWC == 0 (which we change to 0x10000) it transfers, causing an underflow, then asks if it's reached 0
	// since IPU_FROM is beholden to the OUT FIFO, if there's nothing to transfer, it will stay at 0 and won't underflow
	// so the DMA will end.
	if (ipu0ch.qwc == 0x10000)
	{
		ipu0ch.qwc = 0;
		ipu0ch.chcr.STR = false;
		hwDmacIrq(DMAC_FROM_IPU);
	}
}

__fi void dmaIPU1(void) // toIPU
{
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);

	if (ipu1ch.chcr.MOD == CHAIN_MODE)  //Chain Mode
	{
		if(ipu1ch.qwc == 0)
		{
			IPU1Status.InProgress = false;
			IPU1Status.DMAFinished = false;
		}
		else // Attempting to continue a previous chain
		{
			tDMA_TAG tmp;
			tmp._u32 = ipu1ch.chcr._u32;
			IPU1Status.InProgress = true;
			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && ipu1ch.chcr.TIE))
				IPU1Status.DMAFinished = true;
			else
				IPU1Status.DMAFinished = false;
		}
	}
	else // Normal Mode
	{
		IPU1Status.InProgress = true;
		IPU1Status.DMAFinished = true;
	}

	IPU1dma();
}

void ipu0Interrupt(void)
{
	if(ipu0ch.qwc > 0)
	{
		IPU0dma();
		return;
	}

	ipu0ch.chcr.STR = false;
	hwDmacIrq(DMAC_FROM_IPU);
	CPU_SET_DMASTALL(DMAC_FROM_IPU, false);
}

__fi void ipu1Interrupt(void)
{
	if(!IPU1Status.DMAFinished || IPU1Status.InProgress)  //Sanity Check
	{
		IPU1dma();
		return;
	}

	ipu1ch.chcr.STR = false;
	hwDmacIrq(DMAC_TO_IPU);
	CPU_SET_DMASTALL(DMAC_TO_IPU, false);
}

#if MULTI_ISA_COMPILE_ONCE

static constexpr std::array<u8, 1024> make_clip_lut(void)
{
	std::array<u8, 1024> lut = {};
	for (int i = -384; i < 640; i++)
		lut[i+384] = (i < 0) ? 0 : ((i > 255) ? 255 : i);
	return lut;
}

static constexpr mpeg2_scan_pack make_scan_pack(void)
{
	constexpr u8 mpeg2_scan_norm[64] = {
		/* Zig-Zag scan pattern */
		0,  1,  8,  16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
		12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
		35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
		58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
	};

	constexpr u8 mpeg2_scan_alt[64] = {
		/* Alternate scan pattern */
		0,  8,  16, 24,  1,  9,  2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
		41, 33, 26, 18,  3, 11,  4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
		51, 59, 20, 28,  5, 13,  6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
		53, 61, 22, 30,  7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63
	};

	mpeg2_scan_pack pack = {};

	for (int i = 0; i < 64; i++) {
		int j = mpeg2_scan_norm[i];
		pack.norm[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
		j = mpeg2_scan_alt[i];
		pack.alt[i] = ((j & 0x36) >> 1) | ((j & 0x09) << 2);
	}

	return pack;
}

alignas(16) const std::array<u8, 1024> g_idct_clip_lut = make_clip_lut();
alignas(16) const mpeg2_scan_pack mpeg2_scan = make_scan_pack();

#endif


void IPU_Fifo::init()
{
	out.readpos = 0;
	out.writepos = 0;
	in.readpos = 0;
	in.writepos = 0;
	memset(in.data, 0, sizeof(in.data));
	memset(out.data, 0, sizeof(out.data));
}

void IPU_Fifo_Input::clear()
{
	memset(data, 0, sizeof(data));
	g_BP.IFC = 0;
	ipuRegs.ctrl.IFC = 0;
	readpos = 0;
	writepos = 0;

	// Because the FIFO is drained it will request more data immediately
	IPUCoreStatus.DataRequested = true;

	if (ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
	{
		CPU_INT(DMAC_TO_IPU, 4);
	}
}

void IPU_Fifo_Output::clear()
{
	memset(data, 0, sizeof(data));
	ipuRegs.ctrl.OFC = 0;
	readpos = 0;
	writepos = 0;
}

void IPU_Fifo::clear()
{
	in.clear();
	out.clear();
}

int IPU_Fifo_Input::write(const u32* pMem, int size)
{
	const int transfer_size = std::min(size, 8 - (int)g_BP.IFC);
	if (!transfer_size) return 0;

	const int first_words  = std::min((32 - writepos), transfer_size << 2);
	const int second_words = (transfer_size << 2) - first_words;

	memcpy(&data[writepos], pMem, first_words << 2);
	pMem += first_words;

	if(second_words)
		memcpy(&data[0], pMem, second_words << 2);

	writepos = (writepos + (transfer_size << 2)) & 31;

	g_BP.IFC += transfer_size;

	if (g_BP.IFC == 8)
		IPUCoreStatus.DataRequested = false;

	return transfer_size;
}

int IPU_Fifo_Input::read(void *value)
{
	// wait until enough data to ensure proper streaming.
	if (g_BP.IFC <= 1)
	{
		// IPU FIFO is empty and DMA is waiting so lets tell the DMA we are ready to put data in the FIFO
		IPUCoreStatus.DataRequested = true;

		if(ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
		{
			CPU_INT( DMAC_TO_IPU, std::min(8U, ipu1ch.qwc));
		}

		if (g_BP.IFC == 0) return 0;
	}

	const void *src = &data[readpos];
	CopyQWC(value, src);

	readpos = (readpos + 4) & 31;
	g_BP.IFC--;
	return 1;
}

int IPU_Fifo_Output::write(const u32 *value, uint size)
{
	const int transfer_size = std::min(size, 8 - (uint)ipuRegs.ctrl.OFC);
	if(!transfer_size) return 0;

	const int first_words = std::min((32 - writepos), transfer_size << 2);
	const int second_words = (transfer_size << 2) - first_words;

	memcpy(&data[writepos], value, first_words << 2);
	value += first_words;
	if (second_words)
		memcpy(&data[0], value, second_words << 2);

	writepos = (writepos + (transfer_size << 2)) & 31;

	ipuRegs.ctrl.OFC += transfer_size;

	if(ipu0ch.chcr.STR)
		IPU_INT_FROM(1);
	return transfer_size;
}

void IPU_Fifo_Output::read(void *value, uint size)
{
	ipuRegs.ctrl.OFC -= size;

	const int first_words = std::min((32 - readpos), static_cast<int>(size << 2));
	const int second_words = static_cast<int>(size << 2) - first_words;

	memcpy(value, &data[readpos], first_words << 2);
	value = static_cast<u32*>(value) + first_words;

	if (second_words)
		memcpy(value, &data[0], second_words << 2);

	readpos = (readpos + static_cast<int>(size << 2)) & 31;
}

void ReadFIFO_IPUout(mem128_t* out)
{
	if (!( ipuRegs.ctrl.OFC > 0)) return;
	ipu_fifo.out.read(out, 1);

	// Games should always check the fifo before reading from it -- so if the FIFO has no data
	// its either some glitchy game or a bug in pcsx2.
}

void WriteFIFO_IPUin(const mem128_t* value)
{
	//committing every 16 bytes
	if(ipu_fifo.in.write(value->_u32, 1) > 0)
	{
		if (ipuRegs.ctrl.BUSY)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(2 * BIAS);
		}
	}
}

// Also defined in IPU_MultiISA.cpp, but IPU.cpp is not unshared.
// whenever reading fractions of bytes. The low bits always come from the next byte
// while the high bits come from the current byte
__ri static u8 getBits32(u8* address)
{
	if (!g_BP.FillBuffer(32))
		return 0;

	const u8* readpos = &g_BP.internal_qwc->_u8[g_BP.BP / 8];

	if (uint shift = (g_BP.BP & 7))
	{
		u32 mask = (0xff >> shift);
		mask = mask | (mask << 8) | (mask << 16) | (mask << 24);

		*(u32*)address = ((~mask & *(u32*)(readpos + 1)) >> (8 - shift)) | (((mask) & *(u32*)readpos) << shift);
	}
	else
	{
		// Bit position-aligned -- no masking/shifting necessary
		*(u32*)address = *(u32*)readpos;
	}

	return 1;
}

void tIPU_cmd::clear()
{
	memset(this, 0, sizeof(*this));
	current = 0xffffffff;
}

__fi void IPUProcessInterrupt(void)
{
	if (ipuRegs.ctrl.BUSY)
		IPUWorker();
}

/////////////////////////////////////////////////////////
// Register accesses (run on EE thread)

void ipuReset(void)
{
	IPUWorker = MULTI_ISA_SELECT(IPUWorker);
	memset(&ipuRegs, 0, sizeof(ipuRegs));
	memset(&g_BP, 0, sizeof(g_BP));
	memset(&decoder, 0, sizeof(decoder));
	IPUCoreStatus.DataRequested = false;
	IPUCoreStatus.WaitingOnIPUFrom= false;
	IPUCoreStatus.WaitingOnIPUTo = false;

	decoder.picture_structure = FRAME_PICTURE;      //default: progressive...my guess:P

	ipu_fifo.init();
	ipu_cmd.clear();
	ipuDmaReset();
}

bool SaveStateBase::ipuFreeze(void)
{
	// Get a report of the status of the ipu variables when saving and loading savestates.
	if (!(FreezeTag("IPU")))
		return false;

	Freeze(ipu_fifo);

	Freeze(g_BP);
	Freeze(g_ipu_vqclut);
	Freeze(g_ipu_thresh);
	Freeze(coded_block_pattern);
	Freeze(decoder);
	Freeze(ipu_cmd);
	Freeze(IPUCoreStatus);

	return IsOkay();
}


__fi u32 ipuRead32(u32 mem)
{
	mem &= 0xff;	// ipu repeats every 0x100

	switch (mem)
	{
		ipucase(IPU_CMD) : // IPU_CMD
		{
			if (ipu_cmd.CMD != SCE_IPU_FDEC && ipu_cmd.CMD != SCE_IPU_VDEC)
			{
				if (getBits32((u8*)&ipuRegs.cmd.DATA))
					ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
			}
			return ipuRegs.cmd.DATA;
		}

		ipucase(IPU_CTRL): // IPU_CTRL
		{
			ipuRegs.ctrl.IFC = g_BP.IFC;
			ipuRegs.ctrl.CBP = coded_block_pattern;
			return ipuRegs.ctrl._u32;
		}

		ipucase(IPU_BP): // IPU_BP
		{
			ipuRegs.ipubp = g_BP.BP & 0x7f;
			ipuRegs.ipubp |= g_BP.IFC << 8;
			ipuRegs.ipubp |= g_BP.FP << 16;
			return ipuRegs.ipubp;
		}

		default:
			break;
	}

	return psHu32(IPU_CMD + mem);
}

__fi u64 ipuRead64(u32 mem)
{
	mem &= 0xff;	// ipu repeats every 0x100

	switch (mem)
	{
		ipucase(IPU_CMD): // IPU_CMD
		{
			if (ipu_cmd.CMD != SCE_IPU_FDEC && ipu_cmd.CMD != SCE_IPU_VDEC)
			{
				if (getBits32((u8*)&ipuRegs.cmd.DATA))
					ipuRegs.cmd.DATA = BigEndian(ipuRegs.cmd.DATA);
			}

			return ipuRegs.cmd._u64;
		}

		ipucase(IPU_CTRL):
		ipucase(IPU_BP):
		ipucase(IPU_TOP): // IPU_TOP
		default:
			break;
	}
	return psHu64(IPU_CMD + mem);
}

void ipuSoftReset(void)
{
	ipu_fifo.clear();
	memset(&g_BP, 0, sizeof(g_BP));

	coded_block_pattern = 0;
	g_ipu_thresh[0] = 0;
	g_ipu_thresh[1] = 0;

	ipuRegs.ctrl._u32 &= 0x7F33F00;
	ipuRegs.top = 0;
	ipu_cmd.clear();
	ipuRegs.cmd.BUSY = 0;
	ipuRegs.cmd.DATA = 0; // required for Enthusia - Professional Racing after fix, or will freeze at start of next video.

	hwIntcIrq(INTC_IPU); // required for FightBox
}

__fi bool ipuWrite32(u32 mem, u32 value)
{
	mem &= 0xfff;

	switch (mem)
	{
		ipucase(IPU_CMD): // IPU_CMD
			IPUCMD_WRITE(value);
			return false;

		ipucase(IPU_CTRL): // IPU_CTRL
			// CTRL = the first 16 bits of ctrl [0x8000ffff], + value for the next 16 bits,
			// minus the reserved bits. (18-19; 27-29) [0x47f30000]
			ipuRegs.ctrl._u32 = (value & 0x47f30000) | (ipuRegs.ctrl._u32 & 0x8000ffff);
			if (ipuRegs.ctrl.IDP == 3) /* IPU Invalid Intra DC Precision, switching to 9 bits */
				ipuRegs.ctrl.IDP = 1;

			if (ipuRegs.ctrl.RST)
				ipuSoftReset(); // RESET
			return false;
	}
	return true;
}

// returns FALSE when the writeback is handled, TRUE if the caller should do the
// writeback itself.
__fi bool ipuWrite64(u32 mem, u64 value)
{
	mem &= 0xfff;

	switch (mem)
	{
		ipucase(IPU_CMD):
			IPUCMD_WRITE((u32)value);
		return false;
	}

	return true;
}

//////////////////////////////////////////////////////
// IPU Commands (exec on worker thread only)

static void ipuBCLR(u32 val)
{
	ipu_fifo.in.clear();
	memset(&g_BP, 0, sizeof(g_BP));
	g_BP.BP = val & 0x7F;

	ipuRegs.cmd.BUSY = 0;
}

static __ri void ipuIDEC(tIPU_CMD_IDEC idec)
{
	//from IPU_CTRL
	ipuRegs.ctrl.PCT = I_TYPE; //Intra DECoding;)

	decoder.coding_type			= ipuRegs.ctrl.PCT;
	decoder.mpeg1				= ipuRegs.ctrl.MP1;
	decoder.q_scale_type		= ipuRegs.ctrl.QST;
	decoder.intra_vlc_format	= ipuRegs.ctrl.IVF;
	decoder.scantype			= ipuRegs.ctrl.AS;
	decoder.intra_dc_precision	= ipuRegs.ctrl.IDP;

//from IDEC value
	decoder.quantizer_scale		= idec.QSC;
	decoder.frame_pred_frame_dct= !idec.DTD;
	decoder.sgn = idec.SGN;
	decoder.dte = idec.DTE;
	decoder.ofm = idec.OFM;

	//other stuff
	decoder.dcr = 1; // resets DC prediction value
}

static __ri void ipuBDEC(tIPU_CMD_BDEC bdec)
{
	decoder.coding_type			= I_TYPE;
	decoder.mpeg1				= ipuRegs.ctrl.MP1;
	decoder.q_scale_type		= ipuRegs.ctrl.QST;
	decoder.intra_vlc_format	= ipuRegs.ctrl.IVF;
	decoder.scantype			= ipuRegs.ctrl.AS;
	decoder.intra_dc_precision	= ipuRegs.ctrl.IDP;

	//from BDEC value
	decoder.quantizer_scale		= decoder.q_scale_type ? non_linear_quantizer_scale [bdec.QSC] : bdec.QSC << 1;
	decoder.macroblock_modes	= bdec.DT ? DCT_TYPE_INTERLACED : 0;
	decoder.dcr					= bdec.DCR;
	decoder.macroblock_modes	|= bdec.MBI ? MACROBLOCK_INTRA : MACROBLOCK_PATTERN;

	memset(&decoder.mb8, 0, sizeof(decoder.mb8));
	memset(&decoder.mb16, 0, sizeof(decoder.mb16));
}

static void ipuSETTH(u32 val)
{
	g_ipu_thresh[0] = (val & 0x1ff);
	g_ipu_thresh[1] = ((val >> 16) & 0x1ff);
}

// --------------------------------------------------------------------------------------
//  IPU Worker / Dispatcher
// --------------------------------------------------------------------------------------

// When a command is written, we set some various busy flags and clear some other junk.
// The actual decoding will be handled by IPUworker.
__fi void IPUCMD_WRITE(u32 val)
{
	ipuRegs.ctrl.ECD = 0;
	ipuRegs.ctrl.SCD = 0;
	ipu_cmd.clear();
	ipu_cmd.current = val;

	switch (ipu_cmd.CMD)
	{
		// BCLR and SETTH  require no data so they always execute inline:

		case SCE_IPU_BCLR:
			ipuBCLR(val);
			hwIntcIrq(INTC_IPU); //DMAC_TO_IPU
			ipuRegs.ctrl.BUSY = 0;
			return;

		case SCE_IPU_SETTH:
			ipuSETTH(val);
			hwIntcIrq(INTC_IPU);
			ipuRegs.ctrl.BUSY = 0;
			return;

		case SCE_IPU_IDEC:
			{
				tIPU_CMD_IDEC _val;
				g_BP.Advance(val & 0x3F);
				_val._u32       = val;
				ipuIDEC(_val);
				ipuRegs.topbusy = 0x80000000;
			}
			break;

		case SCE_IPU_BDEC:
			{
				tIPU_CMD_BDEC _val;
				g_BP.Advance(val & 0x3F);
				_val._u32       = val;
				ipuBDEC(_val);
				ipuRegs.topbusy = 0x80000000;
			}
			break;

		case SCE_IPU_VDEC:
		case SCE_IPU_FDEC:
			g_BP.Advance(val & 0x3F);
			ipuRegs.cmd.BUSY = 0x80000000;
			ipuRegs.topbusy  = 0x80000000;
			break;

		case SCE_IPU_SETIQ:
			g_BP.Advance(val & 0x3F);
			break;

		case SCE_IPU_SETVQ:
		case SCE_IPU_CSC:
		case SCE_IPU_PACK:
		default:
			break;
	}

	ipuRegs.ctrl.BUSY = 1;

	// Have a short delay immitating the time it takes to run IDEC/BDEC, other commands are near instant.
	// Mana Khemia/Metal Saga start IDEC then change IPU0 expecting there to be a delay before IDEC sends data.
	if (ipu_cmd.CMD == SCE_IPU_IDEC || ipu_cmd.CMD == SCE_IPU_BDEC)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPU_INT_PROCESS(64);
	}
	else
		IPUWorker();
}
