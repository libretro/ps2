/*
 * vlc.h
 * Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 * Modified by Florin for PCSX2 emu
 *
 * This file is part of mpeg2dec, a free MPEG-2 video stream decoder.
 * See http://libmpeg2.sourceforge.net/ for updates.
 *
 * mpeg2dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpeg2dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/* NOTE: While part of this header is originally from libmpeg2, which is GPL - licensed,
 * it's not substantial and does not contain any functions, therefore can be argued
 * not to be a derived work. See http://lkml.iu.edu/hypermail/linux/kernel/0301.1/0362.html
 * The constants themselves can also be argued to be part of the MPEG-2 standard, whose
 * patents expired worldwide in Feb 2020.
 */

#pragma once
#include <cstdint>

#ifdef _MSC_VER
#define VLC_ALIGNED16 __declspec(align(16))
#else
#define VLC_ALIGNED16 __attribute__((aligned(16)))
#endif

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
	std::uint8_t modes;
	std::uint8_t len;
};

struct MVtab
{
	std::uint8_t delta;
	std::uint8_t len;
};

struct DMVtab
{
	std::int8_t dmv;
	std::uint8_t len;
};

struct CBPtab
{
	std::uint8_t cbp;
	std::uint8_t len;
};

struct DCtab
{
	std::uint8_t size;
	std::uint8_t len;
};

struct MBAtab
{
	std::uint8_t mba;
	std::uint8_t len;
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

/* The DCT coefficient tables that used to sit here - the pre-expanded
 * first/next/tab0/tab0a/tab1/tab1a/tab2..tab6 sub-table layout carried
 * over from the mpeg2dec port - are gone.  The coefficient VLCs now
 * live in ipu_dct_codes.h as the standard's own Table B-14/B-15 code
 * assignments, expanded into a flat lookup at compile time by
 * ipu_dct_lut.h. */

#undef VLC_ALIGNED16