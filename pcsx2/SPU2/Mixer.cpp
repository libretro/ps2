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
#include "interpolate_table.h"

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

		pcm = std::clamp<s32>(pcm, -0x8000, 0x7fff);
		*(buffer++) = pcm;

		data = ((*blockbytes) << 24) & 0xF0000000;
		s32 pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * prev1) + 32) >> 6);

		pcm2 = std::clamp<s32>(pcm2, -0x8000, 0x7fff);
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
			has_to_call_irq[i] = true;
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
				has_to_call_irq[i] = true;

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
				has_to_call_irq[i] = true;

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
		pitch     = std::clamp((vc.Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

	pitch     = std::min(pitch, 0x3FFF);
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
			has_to_call_irq[i] = true;
	}
	*GetMemPtr(addr) = value;
}

static void V_VolumeSlide_Update(V_VolumeSlide &vs)
{
	s32 step_size = 7 - vs.Step;

	if (vs.Decr)
		step_size = ~step_size;

	u32 counter_inc = 0x8000 >> std::max(0, vs.Shift - 11);
	s32 level_inc = step_size << std::max(0, 11 - vs.Shift);

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
		counter_inc = std::max<u32>(1, counter_inc);
	vs.Counter += counter_inc;

	// If negative phase "increase" to -0x8000 or "decrease" towards 0
	// Unless in Exp + Decr modes
	if (!(vs.Exp && vs.Decr) && vs.Phase)
		level_inc = -level_inc;

	if (vs.Counter >= 0x8000)
	{
		vs.Counter = 0;

		if (!vs.Decr)
			vs.Value = std::clamp<s32>(vs.Value + level_inc, INT16_MIN, INT16_MAX);
		else
		{
			if (vs.Phase)
			{
				s32 low  = INT16_MIN;
				s32 high = 0;
				if (vs.Exp)
				{
					low  = 0;
					high = INT16_MAX;
				}
				vs.Value = std::clamp<s32>(vs.Value + level_inc, low, high);
			}
			else
				vs.Value = std::clamp<s32>(vs.Value + level_inc, 0, INT16_MAX);
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
	Voices.Dry.Left  = std::clamp(inVoices.Dry.Left, -0x8000, 0x7fff);
	Voices.Dry.Right = std::clamp(inVoices.Dry.Right, -0x8000, 0x7fff);
	Voices.Wet.Left  = std::clamp(inVoices.Wet.Left, -0x8000, 0x7fff);
	Voices.Wet.Right = std::clamp(inVoices.Wet.Right, -0x8000, 0x7fff);

	// Write Mixed results To Output Area
	if (Index == 0)
	{
		spu2M_WriteFast(0x1000 + OutPos, Voices.Dry.Left);
		spu2M_WriteFast(0x1200 + OutPos, Voices.Dry.Right);
		spu2M_WriteFast(0x1400 + OutPos, Voices.Wet.Left);
		spu2M_WriteFast(0x1600 + OutPos, Voices.Wet.Right);
	}
	else
	{
		spu2M_WriteFast(0x1800 + OutPos, Voices.Dry.Left);
		spu2M_WriteFast(0x1A00 + OutPos, Voices.Dry.Right);
		spu2M_WriteFast(0x1C00 + OutPos, Voices.Wet.Left);
		spu2M_WriteFast(0x1E00 + OutPos, Voices.Wet.Right);
	}

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
void Mix(short *out_left, short *out_right)
{
	StereoOut32 Out;
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
		Ext.Left  = std::clamp(Ext.Left, -0x8000, 0x7fff);
		Ext.Right = std::clamp(Ext.Right, -0x8000, 0x7fff);
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
		Out       = Cores[1].ReadInput_HiFi();
	else
	{
		Out.Left  = std::clamp(Out.Left,  -0x8000, 0x7fff);
		Out.Right = std::clamp(Out.Right, -0x8000, 0x7fff);
		Out.Left  = (Out.Left  * Cores[1].MasterVol.Left.Value)  >> 15;
		Out.Right = (Out.Right * Cores[1].MasterVol.Right.Value) >> 15;
	}

	// A simple DC blocking high-pass filter
	// Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
	// The magic number 0x7f5c is ceil(INT16_MAX * 0.995)
	DCFilterOut.Left  = (Out.Left  - DCFilterIn.Left  + std::clamp((0x7f5c * DCFilterOut.Left)  >> 15, -0x8000, 0x7fff));
	DCFilterOut.Right = (Out.Right - DCFilterIn.Right + std::clamp((0x7f5c * DCFilterOut.Right) >> 15, -0x8000, 0x7fff));
	DCFilterIn.Left   = Out.Left;
	DCFilterIn.Right  = Out.Right;

	// Final clamp, take care not to exceed 16 bits from here on
	*out_left         = (int16_t)(std::clamp(DCFilterOut.Left,  -0x8000, 0x7fff));
	*out_right        = (int16_t)(std::clamp(DCFilterOut.Right, -0x8000, 0x7fff));

	// Update AutoDMA output positioning
	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;
}
