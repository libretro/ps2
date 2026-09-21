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

#pragma once

/* The decoder's own state: the block being assembled, the quantiser
 * matrices, and the picture parameters the slice layer reads.
 *
 * This is written whole into savestates, so its layout is fixed.
 *
 * The three accessors at the bottom describe where in the macroblock the
 * output for IPU0 currently sits. Output goes out a quadword at a time and
 * can stall part way, so the position has to survive between calls. */

#include "../../common/Pcsx2Defs.h"
#include "../../common/Pcsx2Types.h"

#include "ipu_macroblock.h"

typedef struct decoder_t {
	/* first, state that carries information from one macroblock to the */
	/* next inside a slice, and is never used outside of mpeg2_slice() */

	/* DCT coefficients - should be kept aligned ! */
	s16 DCTblock[64];

	u8 niq[64];	/* non-intraquant matrix (sequence header) */
	u8 iq[64];	/* intraquant matrix (sequence header) */

	macroblock_8 mb8;
	macroblock_16 mb16;
	macroblock_rgb32 rgb32;
	macroblock_rgb16 rgb16;

	uint ipu0_data;	/* amount of data in the output macroblock (in QWC) */
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
	/* Dither Enable */
	int dte;
	/* Output Format */
	int ofm;
	/* Macroblock type */
	int macroblock_modes;
	/* DC Reset */
	int dcr;
	/* Coded block pattern */
	int coded_block_pattern;

	/* stuff derived from bitstream */

	/* the zigzag scan we're supposed to be using, true for alt, false for normal */
	bool scantype;

	int mpeg1;
} decoder_t;

/* Point the IPU0 output at one of the macroblock buffers. The buffers sit
 * one after another inside decoder_t, so the position is just an index of
 * quadwords from mb8, and the caller names the buffer and its size rather
 * than the template working both out from the type. */
static PCSX2_INLINE void ipu_decoder_set_output(decoder_t *d, const void *obj,
                                                u32 bytes)
{
	d->ipu0_idx  = (u32)(((const u8 *)obj - (const u8 *)&d->mb8) / 16);
	d->ipu0_data = bytes / 16;
}

static PCSX2_INLINE u128 *ipu_decoder_data_ptr(decoder_t *d)
{
	return ((u128 *)&d->mb8) + d->ipu0_idx;
}

static PCSX2_INLINE void ipu_decoder_advance(decoder_t *d, u32 amt)
{
	d->ipu0_idx  += amt;
	d->ipu0_data -= amt;
}
