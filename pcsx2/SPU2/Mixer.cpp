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

#include <algorithm>

#include "Global.h"
#include "spu2.h"

static void __forceinline XA_decode_block(s16* buffer, const s16* block, s32& prev1, s32& prev2)
{
	static const s32 tbl_XA_Factor[16][2] =
	{
		{0, 0},
		{60, 0},
		{115, -52},
		{98, -55},
		{122, -60}};
	const s32 header = *block;
	const s32 shift = (header & 0xF) + 16;
	const int id = header >> 4 & 0xF;
	const s32 pred1 = tbl_XA_Factor[id][0];
	const s32 pred2 = tbl_XA_Factor[id][1];

	const s8* blockbytes = (s8*)&block[1];
	const s8* blockend = &blockbytes[13];

	for (; blockbytes <= blockend; ++blockbytes)
	{
		s32 data = ((*blockbytes) << 28) & 0xF0000000;
		s32 pcm = (data >> shift) + (((pred1 * prev1) + (pred2 * prev2) + 32) >> 6);

		pcm         = (s32)std::min(std::max(pcm, -0x8000), 0x7fff);
		*(buffer++) = pcm;

		data = ((*blockbytes) << 24) & 0xF0000000;
		s32 pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * prev1) + 32) >> 6);

		pcm2        = (s32)std::min(std::max(pcm2, -0x8000), 0x7fff);
		*(buffer++) = pcm2;

		prev2 = pcm;
		prev1 = pcm2;
	}
}

static void __forceinline IncrementNextA(V_Core& thiscore, V_Voice& vc, uint voiceidx)
{
	// Important!  Both cores signal IRQ when an address is read, regardless of
	// which core actually reads the address.

	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && (vc.NextA == Cores[i].IRQA))
			SetIrqCall(i);
	}

	vc.NextA++;
	vc.NextA &= 0xFFFFF;
}

// decoded pcm data, used to cache the decoded data so that it needn't be decoded
// multiple times.  Cache chunks are decoded when the mixer requests the blocks, and
// invalided when DMA transfers and memory writes are performed.
PcmCacheEntry pcm_cache_data[pcm_BlockCount];

// LOOP/END sets the ENDX bit and sets NAX to LSA, and the voice is muted if LOOP is not set
// LOOP seems to only have any effect on the block with LOOP/END set, where it prevents muting the voice
// (the documented requirement that every block in a loop has the LOOP bit set is nonsense according to tests)
// LOOP/START sets LSA to NAX unless LSA was written manually since sound generation started
// (see LoopMode, the method by which this is achieved on the real SPU2 is unknown)
#define XAFLAG_LOOP_END (1ul << 0)
#define XAFLAG_LOOP (1ul << 1)
#define XAFLAG_LOOP_START (1ul << 2)

static __forceinline s32 GetNextDataBuffered(V_Core& thiscore, V_Voice& vc, uint voiceidx)
{
	if ((vc.SCurrent & 3) == 0)
	{
		if (vc.PendingLoopStart)
		{
			if ((Cycles - vc.PlayCycle) >= 4)
			{
				if (vc.LoopCycle < vc.PlayCycle)
				{
					vc.LoopStartA = vc.PendingLoopStartA;
					vc.LoopMode = 1;
				}

				vc.PendingLoopStart = false;
			}
		}
		IncrementNextA(thiscore, vc, voiceidx);

		if ((vc.NextA & 7) == 0) // vc.SCurrent == 24 equivalent
		{
			if (vc.LoopFlags & XAFLAG_LOOP_END)
			{
				thiscore.Regs.ENDX |= (1 << voiceidx);
				vc.NextA = vc.LoopStartA | 1;
				if (!(vc.LoopFlags & XAFLAG_LOOP))
				{
					vc.ADSR.Value = 0;
					vc.ADSR.Phase = PHASE_STOPPED;
				}
			}
			else
				vc.NextA++; // no, don't IncrementNextA here.  We haven't read the header yet.
		}
	}

	if (vc.SCurrent == 28)
	{
		vc.SCurrent = 0;

		// We'll need the loop flags and buffer pointers regardless of cache status:

		for (int i = 0; i < 2; i++)
			if (Cores[i].IRQEnable && Cores[i].IRQA == (vc.NextA & 0xFFFF8))
				SetIrqCall(i);

		s16* memptr = GetMemPtr(vc.NextA & 0xFFFF8);
		vc.LoopFlags = *memptr >> 8; // grab loop flags from the upper byte.

		if ((vc.LoopFlags & XAFLAG_LOOP_START) && !vc.LoopMode)
		{
			vc.LoopStartA = vc.NextA & 0xFFFF8;
			vc.LoopCycle = Cycles;
		}

		const int cacheIdx = vc.NextA / pcm_WordsPerBlock;
		PcmCacheEntry& cacheLine = pcm_cache_data[cacheIdx];
		vc.SBuffer = cacheLine.Sampledata;

		if (cacheLine.Validated && vc.Prev1 == cacheLine.Prev1 && vc.Prev2 == cacheLine.Prev2)
		{
			// Cached block!  Read from the cache directly.
			// Make sure to propagate the prev1/prev2 ADPCM:

			vc.Prev1 = vc.SBuffer[27];
			vc.Prev2 = vc.SBuffer[26];
		}
		else
		{
			// Only flag the cache if it's a non-dynamic memory range.
			if (vc.NextA >= SPU2_DYN_MEMLINE)
			{
				cacheLine.Validated = true;
				cacheLine.Prev1 = vc.Prev1;
				cacheLine.Prev2 = vc.Prev2;
			}

			XA_decode_block(vc.SBuffer, memptr, vc.Prev1, vc.Prev2);
		}
	}

	return vc.SBuffer[vc.SCurrent++];
}

static __forceinline void GetNextDataDummy(V_Core& thiscore, V_Voice& vc, uint voiceidx)
{
	IncrementNextA(thiscore, vc, voiceidx);

	if ((vc.NextA & 7) == 0) // vc.SCurrent == 24 equivalent
	{
		if (vc.LoopFlags & XAFLAG_LOOP_END)
		{
			thiscore.Regs.ENDX |= (1 << voiceidx);
			vc.NextA = vc.LoopStartA | 1;
		}
		else
			vc.NextA++; // no, don't IncrementNextA here.  We haven't read the header yet.
	}

	if (vc.SCurrent == 28)
	{
		for (int i = 0; i < 2; i++)
			if (Cores[i].IRQEnable && Cores[i].IRQA == (vc.NextA & 0xFFFF8))
				SetIrqCall(i);

		vc.LoopFlags = *GetMemPtr(vc.NextA & 0xFFFF8) >> 8; // grab loop flags from the upper byte.

		if ((vc.LoopFlags & XAFLAG_LOOP_START) && !vc.LoopMode)
			vc.LoopStartA = vc.NextA & 0xFFFF8;

		vc.SCurrent = 0;
	}

	vc.SP -= 0x1000 * (4 - (vc.SCurrent & 3));
	vc.SCurrent += 4 - (vc.SCurrent & 3);
}

static void __forceinline UpdatePitch(V_Voice& vc, uint coreidx, uint voiceidx)
{
	s32 pitch;
	// [Air] : re-ordered comparisons: Modulated is much more likely to be zero than voice,
	//   and so the way it was before it's have to check both voice and modulated values
	//   most of the time.  Now it'll just check Modulated and short-circuit past the voice
	//   check (not that it amounts to much, but eh every little bit helps).
	if ((vc.Modulated == 0) || (voiceidx == 0))
		pitch     = vc.Pitch;
	else
		pitch     = std::min(std::max((vc.Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0), 0x3fff);

	if (0x3FFF < pitch)
		pitch = 0x3FFF;
	vc.SP    += pitch;
}

static __forceinline void CalculateADSR(V_Core& thiscore, V_Voice& vc, uint voiceidx)
{
	if (vc.ADSR.Phase == PHASE_STOPPED)
		vc.ADSR.Value = 0;
	else if (!ADSR_Calculate(vc.ADSR, thiscore.Index | (voiceidx << 1)))
	{
		vc.ADSR.Value = 0;
		vc.ADSR.Phase = PHASE_STOPPED;
	}
}

static __forceinline s32 GetVoiceValues(V_Core& thiscore, V_Voice& vc, uint voiceidx)
{
	/*
	 * Table generator from Near, with credit to nocash and Ryphecha
	 * Modified to produce a two dimensional array indexed by [interp_idx][0..3]
	 */
	static int16_t interpTable[256][4] = {
		{0x12C7, 0x59B3, 0x1307, -0x0001},
		{0x1288, 0x59B2, 0x1347, -0x0001},
		{0x1249, 0x59B0, 0x1388, -0x0001},
		{0x120B, 0x59AD, 0x13C9, -0x0001},
		{0x11CD, 0x59A9, 0x140B, -0x0001},
		{0x118F, 0x59A4, 0x144D, -0x0001},
		{0x1153, 0x599E, 0x1490, -0x0001},
		{0x1116, 0x5997, 0x14D4, -0x0001},
		{0x10DB, 0x598F, 0x1517, -0x0001},
		{0x109F, 0x5986, 0x155C, -0x0001},
		{0x1065, 0x597C, 0x15A0, -0x0001},
		{0x102A, 0x5971, 0x15E6, -0x0001},
		{0x0FF1, 0x5965, 0x162C, -0x0001},
		{0x0FB7, 0x5958, 0x1672, -0x0001},
		{0x0F7F, 0x5949, 0x16B9, -0x0001},
		{0x0F46, 0x593A, 0x1700, -0x0001},
		{0x0F0F, 0x592A, 0x1747, 0x0000},
		{0x0ED7, 0x5919, 0x1790, 0x0000},
		{0x0EA1, 0x5907, 0x17D8, 0x0000},
		{0x0E6B, 0x58F4, 0x1821, 0x0000},
		{0x0E35, 0x58E0, 0x186B, 0x0000},
		{0x0E00, 0x58CB, 0x18B5, 0x0000},
		{0x0DCB, 0x58B5, 0x1900, 0x0000},
		{0x0D97, 0x589E, 0x194B, 0x0001},
		{0x0D63, 0x5886, 0x1996, 0x0001},
		{0x0D30, 0x586D, 0x19E2, 0x0001},
		{0x0CFD, 0x5853, 0x1A2E, 0x0001},
		{0x0CCB, 0x5838, 0x1A7B, 0x0002},
		{0x0C99, 0x581C, 0x1AC8, 0x0002},
		{0x0C68, 0x57FF, 0x1B16, 0x0002},
		{0x0C38, 0x57E2, 0x1B64, 0x0003},
		{0x0C07, 0x57C3, 0x1BB3, 0x0003},
		{0x0BD8, 0x57A3, 0x1C02, 0x0003},
		{0x0BA9, 0x5782, 0x1C51, 0x0004},
		{0x0B7A, 0x5761, 0x1CA1, 0x0004},
		{0x0B4C, 0x573E, 0x1CF1, 0x0005},
		{0x0B1E, 0x571B, 0x1D42, 0x0005},
		{0x0AF1, 0x56F6, 0x1D93, 0x0006},
		{0x0AC4, 0x56D1, 0x1DE5, 0x0007},
		{0x0A98, 0x56AB, 0x1E37, 0x0007},
		{0x0A6C, 0x5684, 0x1E89, 0x0008},
		{0x0A40, 0x565B, 0x1EDC, 0x0009},
		{0x0A16, 0x5632, 0x1F2F, 0x0009},
		{0x09EB, 0x5609, 0x1F82, 0x000A},
		{0x09C1, 0x55DE, 0x1FD6, 0x000B},
		{0x0998, 0x55B2, 0x202A, 0x000C},
		{0x096F, 0x5585, 0x207F, 0x000D},
		{0x0946, 0x5558, 0x20D4, 0x000E},
		{0x091E, 0x5529, 0x2129, 0x000F},
		{0x08F7, 0x54FA, 0x217F, 0x0010},
		{0x08D0, 0x54CA, 0x21D5, 0x0011},
		{0x08A9, 0x5499, 0x222C, 0x0012},
		{0x0883, 0x5467, 0x2282, 0x0013},
		{0x085D, 0x5434, 0x22DA, 0x0015},
		{0x0838, 0x5401, 0x2331, 0x0016},
		{0x0813, 0x53CC, 0x2389, 0x0018},
		{0x07EF, 0x5397, 0x23E1, 0x0019},
		{0x07CB, 0x5361, 0x2439, 0x001B},
		{0x07A7, 0x532A, 0x2492, 0x001C},
		{0x0784, 0x52F3, 0x24EB, 0x001E},
		{0x0762, 0x52BA, 0x2545, 0x0020},
		{0x0740, 0x5281, 0x259E, 0x0021},
		{0x071E, 0x5247, 0x25F8, 0x0023},
		{0x06FD, 0x520C, 0x2653, 0x0025},
		{0x06DC, 0x51D0, 0x26AD, 0x0027},
		{0x06BB, 0x5194, 0x2708, 0x0029},
		{0x069B, 0x5156, 0x2763, 0x002C},
		{0x067C, 0x5118, 0x27BE, 0x002E},
		{0x065C, 0x50DA, 0x281A, 0x0030},
		{0x063E, 0x509A, 0x2876, 0x0033},
		{0x061F, 0x505A, 0x28D2, 0x0035},
		{0x0601, 0x5019, 0x292E, 0x0038},
		{0x05E4, 0x4FD7, 0x298B, 0x003A},
		{0x05C7, 0x4F95, 0x29E7, 0x003D},
		{0x05AA, 0x4F52, 0x2A44, 0x0040},
		{0x058E, 0x4F0E, 0x2AA1, 0x0043},
		{0x0572, 0x4EC9, 0x2AFF, 0x0046},
		{0x0556, 0x4E84, 0x2B5C, 0x0049},
		{0x053B, 0x4E3E, 0x2BBA, 0x004D},
		{0x0520, 0x4DF7, 0x2C18, 0x0050},
		{0x0506, 0x4DB0, 0x2C76, 0x0054},
		{0x04EC, 0x4D68, 0x2CD4, 0x0057},
		{0x04D2, 0x4D20, 0x2D33, 0x005B},
		{0x04B9, 0x4CD7, 0x2D91, 0x005F},
		{0x04A0, 0x4C8D, 0x2DF0, 0x0063},
		{0x0488, 0x4C42, 0x2E4F, 0x0067},
		{0x0470, 0x4BF7, 0x2EAE, 0x006B},
		{0x0458, 0x4BAC, 0x2F0D, 0x006F},
		{0x0441, 0x4B5F, 0x2F6C, 0x0074},
		{0x042A, 0x4B13, 0x2FCC, 0x0078},
		{0x0413, 0x4AC5, 0x302B, 0x007D},
		{0x03FC, 0x4A77, 0x308B, 0x0082},
		{0x03E7, 0x4A29, 0x30EA, 0x0087},
		{0x03D1, 0x49D9, 0x314A, 0x008C},
		{0x03BC, 0x498A, 0x31AA, 0x0091},
		{0x03A7, 0x493A, 0x3209, 0x0096},
		{0x0392, 0x48E9, 0x3269, 0x009C},
		{0x037E, 0x4898, 0x32C9, 0x00A1},
		{0x036A, 0x4846, 0x3329, 0x00A7},
		{0x0356, 0x47F4, 0x3389, 0x00AD},
		{0x0343, 0x47A1, 0x33E9, 0x00B3},
		{0x0330, 0x474E, 0x3449, 0x00BA},
		{0x031D, 0x46FA, 0x34A9, 0x00C0},
		{0x030B, 0x46A6, 0x3509, 0x00C7},
		{0x02F9, 0x4651, 0x3569, 0x00CD},
		{0x02E7, 0x45FC, 0x35C9, 0x00D4},
		{0x02D6, 0x45A6, 0x3629, 0x00DB},
		{0x02C4, 0x4550, 0x3689, 0x00E3},
		{0x02B4, 0x44FA, 0x36E8, 0x00EA},
		{0x02A3, 0x44A3, 0x3748, 0x00F2},
		{0x0293, 0x444C, 0x37A8, 0x00FA},
		{0x0283, 0x43F4, 0x3807, 0x0101},
		{0x0273, 0x439C, 0x3867, 0x010A},
		{0x0264, 0x4344, 0x38C6, 0x0112},
		{0x0255, 0x42EB, 0x3926, 0x011B},
		{0x0246, 0x4292, 0x3985, 0x0123},
		{0x0237, 0x4239, 0x39E4, 0x012C},
		{0x0229, 0x41DF, 0x3A43, 0x0135},
		{0x021B, 0x4185, 0x3AA2, 0x013F},
		{0x020D, 0x412A, 0x3B00, 0x0148},
		{0x0200, 0x40D0, 0x3B5F, 0x0152},
		{0x01F2, 0x4074, 0x3BBD, 0x015C},
		{0x01E5, 0x4019, 0x3C1B, 0x0166},
		{0x01D9, 0x3FBD, 0x3C79, 0x0171},
		{0x01CC, 0x3F62, 0x3CD7, 0x017B},
		{0x01C0, 0x3F05, 0x3D35, 0x0186},
		{0x01B4, 0x3EA9, 0x3D92, 0x0191},
		{0x01A8, 0x3E4C, 0x3DEF, 0x019C},
		{0x019C, 0x3DEF, 0x3E4C, 0x01A8},
		{0x0191, 0x3D92, 0x3EA9, 0x01B4},
		{0x0186, 0x3D35, 0x3F05, 0x01C0},
		{0x017B, 0x3CD7, 0x3F62, 0x01CC},
		{0x0171, 0x3C79, 0x3FBD, 0x01D9},
		{0x0166, 0x3C1B, 0x4019, 0x01E5},
		{0x015C, 0x3BBD, 0x4074, 0x01F2},
		{0x0152, 0x3B5F, 0x40D0, 0x0200},
		{0x0148, 0x3B00, 0x412A, 0x020D},
		{0x013F, 0x3AA2, 0x4185, 0x021B},
		{0x0135, 0x3A43, 0x41DF, 0x0229},
		{0x012C, 0x39E4, 0x4239, 0x0237},
		{0x0123, 0x3985, 0x4292, 0x0246},
		{0x011B, 0x3926, 0x42EB, 0x0255},
		{0x0112, 0x38C6, 0x4344, 0x0264},
		{0x010A, 0x3867, 0x439C, 0x0273},
		{0x0101, 0x3807, 0x43F4, 0x0283},
		{0x00FA, 0x37A8, 0x444C, 0x0293},
		{0x00F2, 0x3748, 0x44A3, 0x02A3},
		{0x00EA, 0x36E8, 0x44FA, 0x02B4},
		{0x00E3, 0x3689, 0x4550, 0x02C4},
		{0x00DB, 0x3629, 0x45A6, 0x02D6},
		{0x00D4, 0x35C9, 0x45FC, 0x02E7},
		{0x00CD, 0x3569, 0x4651, 0x02F9},
		{0x00C7, 0x3509, 0x46A6, 0x030B},
		{0x00C0, 0x34A9, 0x46FA, 0x031D},
		{0x00BA, 0x3449, 0x474E, 0x0330},
		{0x00B3, 0x33E9, 0x47A1, 0x0343},
		{0x00AD, 0x3389, 0x47F4, 0x0356},
		{0x00A7, 0x3329, 0x4846, 0x036A},
		{0x00A1, 0x32C9, 0x4898, 0x037E},
		{0x009C, 0x3269, 0x48E9, 0x0392},
		{0x0096, 0x3209, 0x493A, 0x03A7},
		{0x0091, 0x31AA, 0x498A, 0x03BC},
		{0x008C, 0x314A, 0x49D9, 0x03D1},
		{0x0087, 0x30EA, 0x4A29, 0x03E7},
		{0x0082, 0x308B, 0x4A77, 0x03FC},
		{0x007D, 0x302B, 0x4AC5, 0x0413},
		{0x0078, 0x2FCC, 0x4B13, 0x042A},
		{0x0074, 0x2F6C, 0x4B5F, 0x0441},
		{0x006F, 0x2F0D, 0x4BAC, 0x0458},
		{0x006B, 0x2EAE, 0x4BF7, 0x0470},
		{0x0067, 0x2E4F, 0x4C42, 0x0488},
		{0x0063, 0x2DF0, 0x4C8D, 0x04A0},
		{0x005F, 0x2D91, 0x4CD7, 0x04B9},
		{0x005B, 0x2D33, 0x4D20, 0x04D2},
		{0x0057, 0x2CD4, 0x4D68, 0x04EC},
		{0x0054, 0x2C76, 0x4DB0, 0x0506},
		{0x0050, 0x2C18, 0x4DF7, 0x0520},
		{0x004D, 0x2BBA, 0x4E3E, 0x053B},
		{0x0049, 0x2B5C, 0x4E84, 0x0556},
		{0x0046, 0x2AFF, 0x4EC9, 0x0572},
		{0x0043, 0x2AA1, 0x4F0E, 0x058E},
		{0x0040, 0x2A44, 0x4F52, 0x05AA},
		{0x003D, 0x29E7, 0x4F95, 0x05C7},
		{0x003A, 0x298B, 0x4FD7, 0x05E4},
		{0x0038, 0x292E, 0x5019, 0x0601},
		{0x0035, 0x28D2, 0x505A, 0x061F},
		{0x0033, 0x2876, 0x509A, 0x063E},
		{0x0030, 0x281A, 0x50DA, 0x065C},
		{0x002E, 0x27BE, 0x5118, 0x067C},
		{0x002C, 0x2763, 0x5156, 0x069B},
		{0x0029, 0x2708, 0x5194, 0x06BB},
		{0x0027, 0x26AD, 0x51D0, 0x06DC},
		{0x0025, 0x2653, 0x520C, 0x06FD},
		{0x0023, 0x25F8, 0x5247, 0x071E},
		{0x0021, 0x259E, 0x5281, 0x0740},
		{0x0020, 0x2545, 0x52BA, 0x0762},
		{0x001E, 0x24EB, 0x52F3, 0x0784},
		{0x001C, 0x2492, 0x532A, 0x07A7},
		{0x001B, 0x2439, 0x5361, 0x07CB},
		{0x0019, 0x23E1, 0x5397, 0x07EF},
		{0x0018, 0x2389, 0x53CC, 0x0813},
		{0x0016, 0x2331, 0x5401, 0x0838},
		{0x0015, 0x22DA, 0x5434, 0x085D},
		{0x0013, 0x2282, 0x5467, 0x0883},
		{0x0012, 0x222C, 0x5499, 0x08A9},
		{0x0011, 0x21D5, 0x54CA, 0x08D0},
		{0x0010, 0x217F, 0x54FA, 0x08F7},
		{0x000F, 0x2129, 0x5529, 0x091E},
		{0x000E, 0x20D4, 0x5558, 0x0946},
		{0x000D, 0x207F, 0x5585, 0x096F},
		{0x000C, 0x202A, 0x55B2, 0x0998},
		{0x000B, 0x1FD6, 0x55DE, 0x09C1},
		{0x000A, 0x1F82, 0x5609, 0x09EB},
		{0x0009, 0x1F2F, 0x5632, 0x0A16},
		{0x0009, 0x1EDC, 0x565B, 0x0A40},
		{0x0008, 0x1E89, 0x5684, 0x0A6C},
		{0x0007, 0x1E37, 0x56AB, 0x0A98},
		{0x0007, 0x1DE5, 0x56D1, 0x0AC4},
		{0x0006, 0x1D93, 0x56F6, 0x0AF1},
		{0x0005, 0x1D42, 0x571B, 0x0B1E},
		{0x0005, 0x1CF1, 0x573E, 0x0B4C},
		{0x0004, 0x1CA1, 0x5761, 0x0B7A},
		{0x0004, 0x1C51, 0x5782, 0x0BA9},
		{0x0003, 0x1C02, 0x57A3, 0x0BD8},
		{0x0003, 0x1BB3, 0x57C3, 0x0C07},
		{0x0003, 0x1B64, 0x57E2, 0x0C38},
		{0x0002, 0x1B16, 0x57FF, 0x0C68},
		{0x0002, 0x1AC8, 0x581C, 0x0C99},
		{0x0002, 0x1A7B, 0x5838, 0x0CCB},
		{0x0001, 0x1A2E, 0x5853, 0x0CFD},
		{0x0001, 0x19E2, 0x586D, 0x0D30},
		{0x0001, 0x1996, 0x5886, 0x0D63},
		{0x0001, 0x194B, 0x589E, 0x0D97},
		{0x0000, 0x1900, 0x58B5, 0x0DCB},
		{0x0000, 0x18B5, 0x58CB, 0x0E00},
		{0x0000, 0x186B, 0x58E0, 0x0E35},
		{0x0000, 0x1821, 0x58F4, 0x0E6B},
		{0x0000, 0x17D8, 0x5907, 0x0EA1},
		{0x0000, 0x1790, 0x5919, 0x0ED7},
		{0x0000, 0x1747, 0x592A, 0x0F0F},
		{-0x0001, 0x1700, 0x593A, 0x0F46},
		{-0x0001, 0x16B9, 0x5949, 0x0F7F},
		{-0x0001, 0x1672, 0x5958, 0x0FB7},
		{-0x0001, 0x162C, 0x5965, 0x0FF1},
		{-0x0001, 0x15E6, 0x5971, 0x102A},
		{-0x0001, 0x15A0, 0x597C, 0x1065},
		{-0x0001, 0x155C, 0x5986, 0x109F},
		{-0x0001, 0x1517, 0x598F, 0x10DB},
		{-0x0001, 0x14D4, 0x5997, 0x1116},
		{-0x0001, 0x1490, 0x599E, 0x1153},
		{-0x0001, 0x144D, 0x59A4, 0x118F},
		{-0x0001, 0x140B, 0x59A9, 0x11CD},
		{-0x0001, 0x13C9, 0x59AD, 0x120B},
		{-0x0001, 0x1388, 0x59B0, 0x1249},
		{-0x0001, 0x1347, 0x59B2, 0x1288},
		{-0x0001, 0x1307, 0x59B3, 0x12C7},
	};

	while (vc.SP >= 0)
	{
		vc.PV4 = vc.PV3;
		vc.PV3 = vc.PV2;
		vc.PV2 = vc.PV1;
		vc.PV1 = GetNextDataBuffered(thiscore, vc, voiceidx);
		vc.SP -= 0x1000;
	}

	const s32 mu = vc.SP + 0x1000;
	s32 pv4      = vc.PV4;
	s32 pv3      = vc.PV3;
	s32 pv2      = vc.PV2;
	s32 pv1      = vc.PV1;
	s32   i      = (mu & 0x0ff0) >> 4;

	return (s32)(
	   ((interpTable[i][0] * pv4) >> 15)
	 + ((interpTable[i][1] * pv3) >> 15)
	 + ((interpTable[i][2] * pv2) >> 15)
	 + ((interpTable[i][3] * pv1) >> 15));
}

// This is Dr. Hell's noise algorithm as implemented in pcsxr
// Supposedly this is 100% accurate
static __forceinline void UpdateNoise(V_Core& thiscore)
{
	static const uint8_t noise_add[64] = {
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1};

	static const uint16_t noise_freq_add[5] = {
		0, 84, 140, 180, 210};


	u32 level = 0x8000 >> (thiscore.NoiseClk >> 2);
	level <<= 16;

	thiscore.NoiseCnt += 0x10000;

	thiscore.NoiseCnt += noise_freq_add[thiscore.NoiseClk & 3];
	if ((thiscore.NoiseCnt & 0xffff) >= noise_freq_add[4])
	{
		thiscore.NoiseCnt += 0x10000;
		thiscore.NoiseCnt -= noise_freq_add[thiscore.NoiseClk & 3];
	}

	if (thiscore.NoiseCnt >= level)
	{
		while (thiscore.NoiseCnt >= level)
			thiscore.NoiseCnt -= level;

		thiscore.NoiseOut = (thiscore.NoiseOut << 1) | noise_add[(thiscore.NoiseOut >> 10) & 63];
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
//                                                                                     //

// writes a signed value to the SPU2 ram
// Performs no cache invalidation -- use only for dynamic memory ranges
// of the SPU2 (between 0x0000 and SPU2_DYN_MEMLINE)
static __forceinline void spu2M_WriteFast(u32 addr, s16 value)
{
	// Fixes some of the oldest hangs in pcsx2's history! :p
	for (int i = 0; i < 2; i++)
	{
		if (Cores[i].IRQEnable && Cores[i].IRQA == addr)
			SetIrqCall(i);
	}
	*GetMemPtr(addr) = value;
}

static void V_VolumeSlide_Update(V_VolumeSlide &vs)
{
	s32 step_size = 7 - vs.Step;

	if (vs.Decr)
		step_size = ~step_size;

	u32 counter_inc = 0x8000;
	if (0 < (vs.Shift - 11))
		counter_inc >>= (vs.Shift - 11);
	s32 level_inc = step_size;
	if (0 < (11 - vs.Shift))
		level_inc <<= (11 - vs.Shift);

	if (vs.Exp)
	{
		if (vs.Decr)
			level_inc = (s16)((level_inc * vs.Value) >> 15);
		else if (vs.Value > 0x6000)
			counter_inc >>= 2;
	}

	// Allow counter_inc to be zero only in when all bits
	// of the rate field are set
	if (vs.Step != 3 && vs.Shift != 0x1f)
		counter_inc = (1 < counter_inc) ? counter_inc : 1;
	vs.Counter += counter_inc;

	// If negative phase "increase" to -0x8000 or "decrease" towards 0
	// Unless in Exp + Decr modes
	if (!(vs.Exp && vs.Decr) && vs.Phase)
		level_inc = -level_inc;

	if (vs.Counter >= 0x8000)
	{
		vs.Counter = 0;

		if (!vs.Decr)
		{
			s32 a    = (((vs.Value + level_inc) < INT16_MIN) ? (vs.Value + level_inc) : INT16_MIN);
			s32 b    = INT16_MAX;
			vs.Value = static_cast<s32>(std::min(a, b));
		}
		else
		{
			s32 low  = vs.Phase ? INT16_MIN : 0;
			s32 high = vs.Phase ? 0 : INT16_MAX;
			if (vs.Exp)
			{
				low  = 0;
				high = INT16_MAX;
			}
			vs.Value = (s32)std::min(std::max(vs.Value + level_inc, low), high);
		}
	}
}

static __forceinline StereoOut32 MixVoice(V_Core& thiscore, V_Voice& vc, uint coreidx, uint voiceidx)
{
	StereoOut32 voiceOut;
	s32 Value      = 0;

	// Most games don't use much volume slide effects.  So only call the UpdateVolume
	// methods when needed by checking the flag outside the method here...
	// (Note: Ys 6 : Ark of Nephistm uses these effects)
	
	if (vc.Volume.Left.Enable)
		V_VolumeSlide_Update(vc.Volume.Left);
	if (vc.Volume.Right.Enable)
		V_VolumeSlide_Update(vc.Volume.Right);

	// SPU2 Note: The spu2 continues to process voices for eternity, always, so we
	// have to run through all the motions of updating the voice regardless of it's
	// audible status.  Otherwise IRQs might not trigger and emulation might fail.

	UpdatePitch(vc, coreidx, voiceidx);

	voiceOut.Left  = 0;
	voiceOut.Right = 0;

	if (vc.ADSR.Phase > PHASE_STOPPED)
	{
		if (vc.Noise)
			Value = (s16)thiscore.NoiseOut;
		else
			Value = GetVoiceValues(thiscore, vc, voiceidx);

		// Update and Apply ADSR  (applies to normal and noise sources)

		CalculateADSR(thiscore, vc, voiceidx);
		Value     = (Value * vc.ADSR.Value) >> 15;
		vc.OutX   = Value;

		voiceOut.Left   = (Value * vc.Volume.Left.Value)  >> 15;
		voiceOut.Right  = (Value * vc.Volume.Right.Value) >> 15;
	}
	else
	{
		while (vc.SP >= 0)
			GetNextDataDummy(thiscore, vc, voiceidx); // Dummy is enough
	}

	// Write-back of raw voice data (post ADSR applied)
	if (voiceidx == 1)
		spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + OutPos, Value);
	else if (voiceidx == 3)
		spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + OutPos, Value);

	return voiceOut;
}

static __forceinline void MixCoreVoices(VoiceMixSet& dest, const uint coreidx)
{
	V_Core& thiscore(Cores[coreidx]);

	for (uint voiceidx = 0; voiceidx < V_Core::NumVoices; ++voiceidx)
	{
		V_Voice& vc(thiscore.Voices[voiceidx]);
		StereoOut32 VVal(MixVoice(thiscore, vc, coreidx, voiceidx));

		// Note: Results from MixVoice are ranged at 16 bits.

		dest.Dry.Left  += VVal.Left  & thiscore.VoiceGates[voiceidx].DryL;
		dest.Dry.Right += VVal.Right & thiscore.VoiceGates[voiceidx].DryR;
		dest.Wet.Left  += VVal.Left  & thiscore.VoiceGates[voiceidx].WetL;
		dest.Wet.Right += VVal.Right & thiscore.VoiceGates[voiceidx].WetR;
	}
}

StereoOut32 V_Core::Mix(const VoiceMixSet& inVoices, const StereoOut32& Input, const StereoOut32& Ext)
{
	StereoOut32 TD;
	VoiceMixSet Voices;
	if (MasterVol.Left.Enable)
		V_VolumeSlide_Update(MasterVol.Left);
	if (MasterVol.Right.Enable)
		V_VolumeSlide_Update(MasterVol.Right);
	UpdateNoise(*this);

	// Saturate final result to standard 16 bit range.
	Voices.Dry.Left  = std::min(std::max(inVoices.Dry.Left, -0x8000), 0x7fff);
	Voices.Dry.Right = std::min(std::max(inVoices.Dry.Right, -0x8000), 0x7fff);
	Voices.Wet.Left  = std::min(std::max(inVoices.Wet.Left, -0x8000), 0x7fff);
	Voices.Wet.Right = std::min(std::max(inVoices.Wet.Right, -0x8000), 0x7fff);

	// Write Mixed results To Output Area
	spu2M_WriteFast(((0 == Index) ? 0x1000 : 0x1800) + OutPos, Voices.Dry.Left);
	spu2M_WriteFast(((0 == Index) ? 0x1200 : 0x1A00) + OutPos, Voices.Dry.Right);
	spu2M_WriteFast(((0 == Index) ? 0x1400 : 0x1C00) + OutPos, Voices.Wet.Left);
	spu2M_WriteFast(((0 == Index) ? 0x1600 : 0x1E00) + OutPos, Voices.Wet.Right);

	// Mix in the Input data
	TD.Left   = Input.Left & DryGate.InpL;
	TD.Right  = Input.Right & DryGate.InpR;

	// Mix in the Voice data
	TD.Left  += Voices.Dry.Left & DryGate.SndL;
	TD.Right += Voices.Dry.Right & DryGate.SndR;

	// Mix in the External (nothing/core0) data
	TD.Left  += Ext.Left & DryGate.ExtL;
	TD.Right += Ext.Right & DryGate.ExtR;

	// ----------------------------------------------------------------------------
	//    Reverberation Effects Processing
	// ----------------------------------------------------------------------------
	// SPU2 has an FxEnable bit which seems to disable all reverb processing *and*
	// output, but does *not* disable the advancing buffers.  IRQs are not triggered
	// and reverb is rendered silent.
	//
	// Technically we should advance the buffers even when fx are disabled.  However
	// there are two things that make this very unlikely to matter:
	//
	//  1. Any SPU2 app wanting to avoid noise or pops needs to clear the reverb buffers
	//     when adjusting settings anyway; so the read/write positions in the reverb
	//     buffer after FxEnabled is set back to 1 doesn't really matter.
	//
	//  2. Writes to ESA (and possibly EEA) reset the buffer pointers to 0.
	//
	// On the other hand, updating the buffer is cheap and easy, so might as well. ;)

	StereoOut32 TW;

	// Mix Input, Voice, and External data:

	TW.Left = Input.Left & WetGate.InpL;
	TW.Right = Input.Right & WetGate.InpR;

	TW.Left += Voices.Wet.Left & WetGate.SndL;
	TW.Right += Voices.Wet.Right & WetGate.SndR;
	TW.Left += Ext.Left & WetGate.ExtL;
	TW.Right += Ext.Right & WetGate.ExtR;

	StereoOut32 RV  = DoReverb(TW);

	// Mix Dry + Wet
	// (master volume is applied later to the result of both outputs added together).
	TD.Left  += (RV.Left  * FxVol.Left)  >> 15;
	TD.Right += (RV.Right * FxVol.Right) >> 15;
	return TD;
}

// Gcc does not want to inline it when lto is enabled because some functions growth too much.
// The function is big enought to see any speed impact. -- Gregory
#ifndef __POSIX__
__forceinline
#endif
	void
	Mix()
{
	StereoOut32 Out;
	StereoOut16 OutS16;
	StereoOut32 empty;
	StereoOut32 Ext;
	// Note: Playmode 4 is SPDIF, which overrides other inputs.
	StereoOut32 InputData[2];
	// SPDIF is on Core 0:
	// Fixme:
	// 1. We do not have an AC3 decoder for the bitstream.
	// 2. Games usually provide a normal ADMA stream as well and want to see it getting read!

	empty.Left = empty.Right = 0;
	if (PlayMode & 8)
		InputData[1] = empty;
	else
	{
		const StereoOut32& data = Cores[1].ReadInput();
		InputData[1].Left  = (data.Left  * Cores[1].InpVol.Left)  >> 15;
		InputData[1].Right = (data.Right * Cores[1].InpVol.Right) >> 15;
	}
	{
		const StereoOut32& data = Cores[0].ReadInput();
		InputData[0].Left  = (data.Left  * Cores[0].InpVol.Left)  >> 15;
		InputData[0].Right = (data.Right * Cores[0].InpVol.Right) >> 15;
	}

	// Todo: Replace me with memzero initializer!
	VoiceMixSet VoiceData[2]; // mixed voice data for each core.
	VoiceData[0].Dry.Left    = 0;
	VoiceData[0].Dry.Right   = 0;
	VoiceData[0].Wet.Left    = 0;
	VoiceData[0].Wet.Right   = 0;
	VoiceData[1].Dry.Left    = 0;
	VoiceData[1].Dry.Right   = 0;
	VoiceData[1].Wet.Left    = 0;
	VoiceData[1].Wet.Right   = 0;
	MixCoreVoices(VoiceData[0], 0);
	MixCoreVoices(VoiceData[1], 1);

	Ext = Cores[0].Mix(VoiceData[0], InputData[0], empty);

	if ((PlayMode & 4) || (Cores[0].Mute != 0))
		Ext = empty;
	else
	{
		Ext.Left  = std::min(std::max(Ext.Left,  -0x8000), 0x7fff);
		Ext.Right = std::min(std::max(Ext.Right, -0x8000), 0x7fff);
		Ext.Left  = (Ext.Left  * Cores[0].MasterVol.Left.Value)  >> 15;
		Ext.Right = (Ext.Right * Cores[0].MasterVol.Right.Value) >> 15;
	}

	// Commit Core 0 output to ram before mixing Core 1:
	spu2M_WriteFast(0x800 + OutPos, Ext.Left);
	spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

	Ext.Left  = (Ext.Left  * Cores[1].ExtVol.Left)  >> 15;
	Ext.Right = (Ext.Right * Cores[1].ExtVol.Right) >> 15;
	Out       = Cores[1].Mix(VoiceData[1], InputData[1], Ext);

	// Experimental CDDA support
	// The CDDA overrides all other mixer output.  It's a direct feed!
	if (PlayMode & 8)
		Out = Cores[1].ReadInput_HiFi();
	else
	{
		Out.Left  = std::min(std::max(Out.Left,  -0x8000), 0x7fff);
		Out.Right = std::min(std::max(Out.Right, -0x8000), 0x7fff);
		Out.Left  = (Out.Left  * Cores[1].MasterVol.Left.Value)  >> 15;
		Out.Right = (Out.Right * Cores[1].MasterVol.Right.Value) >> 15;
	}

	// A simple DC blocking high-pass filter
	// Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
	// The magic number 0x7f5c is ceil(INT16_MAX * 0.995)
	DCFilterOut.Left  = (Out.Left - DCFilterIn.Left   + std::min(std::max((0x7f5c * DCFilterOut.Left)  >> 15, -0x8000), 0x7fff));
	DCFilterOut.Right = (Out.Right - DCFilterIn.Right + std::min(std::max((0x7f5c * DCFilterOut.Right) >> 15, -0x8000), 0x7fff));
	DCFilterIn.Left   = Out.Left;
	DCFilterIn.Right  = Out.Right;

	Out.Left          = DCFilterOut.Left;
	Out.Right         = DCFilterOut.Right;

	// Final clamp, take care not to exceed 16 bits from here on
	Out.Left          = std::min(std::max(Out.Left, -0x8000), 0x7fff);
	Out.Right         = std::min(std::max(Out.Right, -0x8000), 0x7fff);

	OutS16.Left       = (s16)Out.Left;
	OutS16.Right      = (s16)Out.Right;

	SndBuffer::Write(OutS16);

	// Update AutoDMA output positioning
	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;
}
