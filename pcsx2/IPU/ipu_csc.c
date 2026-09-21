/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "ipu_csc.h"
#include "yuv2rgb.h"

void ipu_csc(const macroblock_8 *mb8, macroblock_rgb32 *rgb32, int sgn,
             const u16 *thresh)
{
	int y, x;

	yuv2rgb(mb8, rgb32);

	/* Walk the pixels as the struct declares them: the channels are named
	 * fields, so there is no cast to make and nothing for the aliasing
	 * rules to object to. Doing it a word at a time also made the sign
	 * flip depend on byte order, since the mask named the low three bytes
	 * rather than the three colour channels. */
	if (thresh[0] > 0)
	{
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
			{
				if (rgb32->c[y][x].r < thresh[0]
				 && rgb32->c[y][x].g < thresh[0]
				 && rgb32->c[y][x].b < thresh[0])
				{
					rgb32->c[y][x].r = 0;
					rgb32->c[y][x].g = 0;
					rgb32->c[y][x].b = 0;
					rgb32->c[y][x].a = 0;
				}
				else if (rgb32->c[y][x].r < thresh[1]
				      && rgb32->c[y][x].g < thresh[1]
				      && rgb32->c[y][x].b < thresh[1])
					rgb32->c[y][x].a = 0x40;
			}
	}
	else if (thresh[1] > 0)
	{
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
				if (rgb32->c[y][x].r < thresh[1]
				 && rgb32->c[y][x].g < thresh[1]
				 && rgb32->c[y][x].b < thresh[1])
					rgb32->c[y][x].a = 0x40;
	}

	if (sgn)
	{
		/* Only the three colour channels flip; alpha keeps whatever the
		 * threshold pass left. */
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
			{
				rgb32->c[y][x].r ^= 0x80;
				rgb32->c[y][x].g ^= 0x80;
				rgb32->c[y][x].b ^= 0x80;
			}
	}
}

static u8 closest_index(const macroblock_rgb16 *rgb16, const rgb16_t *clut,
                        int i, int j)
{
	u8 index = 0;
	int min_distance = 0x7fffffff;
	int k;

	for (k = 0; k < 16; k++)
	{
		const int dr = (int)rgb16->c[i][j].r - (int)clut[k].r;
		const int dg = (int)rgb16->c[i][j].g - (int)clut[k].g;
		const int db = (int)rgb16->c[i][j].b - (int)clut[k].b;
		const int distance = dr * dr + dg * dg + db * db;

		/* Ties: the manual (8.6.3) describes a sequential scan
		 * i=0..15 taking the minimum, so the lowest index wins -
		 * which the strict > below implements. */
		if (min_distance > distance)
		{
			index = (u8)k;
			min_distance = distance;
		}
	}

	return index;
}

void ipu_vq(const macroblock_rgb16 *rgb16, u8 *indx4, const rgb16_t *clut)
{
	int i, j;

	for (i = 0; i < 16; i++)
		for (j = 0; j < 8; j++)
			indx4[i * 8 + j] =
				(u8)((closest_index(rgb16, clut, i, 2 * j + 1) << 4)
				   |  closest_index(rgb16, clut, i, 2 * j));
}
