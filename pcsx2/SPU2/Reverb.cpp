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

#include <array>

#include "Global.h"
#include "../GS/GSVector.h"

StereoOut32 V_Core_DoReverb(V_Core *c, StereoOut32 Input)
{
	if (c->EffectsStartA >= c->EffectsEndA)
	{
		StereoOut32 ret;
		ret.Left = ret.Right = 0;
		return ret;
	}

	/* The reverb work-area bounds and the modulo divisor are loop-invariant
	 * for the duration of this call: they derive only from EffectsStartA /
	 * EffectsEndA (written via the register table, never inside the mixer)
	 * and from Cycles, which is constant across one DoReverb. Compute them
	 * once here instead of re-deriving the masks on each of the 14 indexer
	 * calls below. Arithmetic is identical to the previous per-call form,
	 * so the produced indices are bit-for-bit unchanged. */
	const u32 rv_start = c->EffectsStartA & 0x3fffff;
	const u32 rv_end   = (c->EffectsEndA & 0x3fffff) | 0xffff;
	const u32 rv_size  = (rv_end - rv_start) + 1;
	const u32 rv_phase = Cycles >> 1;
	auto Indexer = [rv_start, rv_size, rv_phase](s32 offset) -> u32
	{
		u32 x = (rv_phase + (u32)offset) % rv_size;
		return ((x + rv_start) & 0xfffff);
	};

	Input.Left  = pcsx2_clamp_i(Input.Left, -0x8000, 0x7fff);
	Input.Right = pcsx2_clamp_i(Input.Right, -0x8000, 0x7fff);

	c->RevbDownBuf[0][c->RevbSampleBufPos] = Input.Left;
	c->RevbDownBuf[1][c->RevbSampleBufPos] = Input.Right;
	c->RevbDownBuf[0][c->RevbSampleBufPos | 64] = Input.Left;
	c->RevbDownBuf[1][c->RevbSampleBufPos | 64] = Input.Right;

	bool R = Cycles & 1;

	// Calculate the read/write addresses we'll be needing for this session of reverb.

	const u32 same_src = Indexer(R ? c->Revb.SAME_R_SRC : c->Revb.SAME_L_SRC);
	const u32 same_dst = Indexer(R ? c->Revb.SAME_R_DST : c->Revb.SAME_L_DST);
	const u32 same_prv = Indexer(R ? c->Revb.SAME_R_DST - 1 : c->Revb.SAME_L_DST - 1);

	const u32 diff_src = Indexer(R ? c->Revb.DIFF_L_SRC : c->Revb.DIFF_R_SRC);
	const u32 diff_dst = Indexer(R ? c->Revb.DIFF_R_DST : c->Revb.DIFF_L_DST);
	const u32 diff_prv = Indexer(R ? c->Revb.DIFF_R_DST - 1 : c->Revb.DIFF_L_DST - 1);

	const u32 comb1_src = Indexer(R ? c->Revb.COMB1_R_SRC : c->Revb.COMB1_L_SRC);
	const u32 comb2_src = Indexer(R ? c->Revb.COMB2_R_SRC : c->Revb.COMB2_L_SRC);
	const u32 comb3_src = Indexer(R ? c->Revb.COMB3_R_SRC : c->Revb.COMB3_L_SRC);
	const u32 comb4_src = Indexer(R ? c->Revb.COMB4_R_SRC : c->Revb.COMB4_L_SRC);

	const u32 apf1_src = Indexer(R ? (c->Revb.APF1_R_DST - c->Revb.APF1_SIZE) : (c->Revb.APF1_L_DST - c->Revb.APF1_SIZE));
	const u32 apf1_dst = Indexer(R ? c->Revb.APF1_R_DST : c->Revb.APF1_L_DST);
	const u32 apf2_src = Indexer(R ? (c->Revb.APF2_R_DST - c->Revb.APF2_SIZE) : (c->Revb.APF2_L_DST - c->Revb.APF2_SIZE));
	const u32 apf2_dst = Indexer(R ? c->Revb.APF2_R_DST : c->Revb.APF2_L_DST);

	// -----------------------------------------
	//          Optimized IRQ Testing !
	// -----------------------------------------

	// This test is enhanced by using the reverb effects area begin/end test as a
	// shortcut, since all buffer addresses are within that area.  If the IRQA isn't
	// within that zone then the "bulk" of the test is skipped, so this should only
	// be a slowdown on a few evil games.

	for (int i = 0; has_irq_armed && i < 2; i++)
	{
		if (c->FxEnable && Cores[i].IRQEnable && ((Cores[i].IRQA >= c->EffectsStartA) && (Cores[i].IRQA <= c->EffectsEndA)))
		{
			if ((Cores[i].IRQA == same_src) || (Cores[i].IRQA == diff_src) ||
				(Cores[i].IRQA == same_dst) || (Cores[i].IRQA == diff_dst) ||
				(Cores[i].IRQA == same_prv) || (Cores[i].IRQA == diff_prv) ||

				(Cores[i].IRQA == comb1_src) || (Cores[i].IRQA == comb2_src) ||
				(Cores[i].IRQA == comb3_src) || (Cores[i].IRQA == comb4_src) ||

				(Cores[i].IRQA == apf1_dst) || (Cores[i].IRQA == apf1_src) ||
				(Cores[i].IRQA == apf2_dst) || (Cores[i].IRQA == apf2_src))
				{ has_to_call_irq[i] = true; }
		}
	}

	// Reverb algorithm pretty much directly ripped from http://drhell.web.fc2.com/ps1/
	// minus the 35 step FIR which just seems to break things.

	s32 apf2;

#define MUL(x, y) ((x) * (y) >> 15)
	s32 in   = MUL(R ? c->Revb.IN_COEF_R : c->Revb.IN_COEF_L, ReverbDownsample(*c, R));

	s32 same = MUL(c->Revb.IIR_VOL, in + MUL(c->Revb.WALL_VOL, _spu2mem[same_src]) - _spu2mem[same_prv]) + _spu2mem[same_prv];
	s32 diff = MUL(c->Revb.IIR_VOL, in + MUL(c->Revb.WALL_VOL, _spu2mem[diff_src]) - _spu2mem[diff_prv]) + _spu2mem[diff_prv];

	s32 out  = MUL(c->Revb.COMB1_VOL, _spu2mem[comb1_src]) + MUL(c->Revb.COMB2_VOL, _spu2mem[comb2_src]) + MUL(c->Revb.COMB3_VOL, _spu2mem[comb3_src]) + MUL(c->Revb.COMB4_VOL, _spu2mem[comb4_src]);

	s32 apf1 = out - MUL(c->Revb.APF1_VOL, _spu2mem[apf1_src]);
	out      = _spu2mem[apf1_src] + MUL(c->Revb.APF1_VOL, apf1);
	apf2     = out - MUL(c->Revb.APF2_VOL, _spu2mem[apf2_src]);
	out      = _spu2mem[apf2_src] + MUL(c->Revb.APF2_VOL, apf2);

	// According to no$psx the effects always run but don't always write back, see check in V_Core::Mix
	if (c->FxEnable)
	{
		_spu2mem[same_dst] = pcsx2_clamp_i(same, -0x8000, 0x7fff);
		_spu2mem[diff_dst] = pcsx2_clamp_i(diff, -0x8000, 0x7fff);
		_spu2mem[apf1_dst] = pcsx2_clamp_i(apf1, -0x8000, 0x7fff);
		_spu2mem[apf2_dst] = pcsx2_clamp_i(apf2, -0x8000, 0x7fff);
	}

	out = pcsx2_clamp_i(out, -0x8000, 0x7fff);

	c->RevbUpBuf[R][c->RevbSampleBufPos] = out;
	c->RevbUpBuf[!R][c->RevbSampleBufPos] = 0;

	c->RevbUpBuf[R][c->RevbSampleBufPos | 64] = out;
	c->RevbUpBuf[!R][c->RevbSampleBufPos | 64] = 0;

	c->RevbSampleBufPos = (c->RevbSampleBufPos + 1) & 63;

	return ReverbUpsample(*c);
}
