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

#include "PrecompiledHeader.h"

#include <algorithm>

#include "Global.h"
#include "spu2.h"
#include "interpolate_table.h"

static const s32 tbl_XA_Factor[16][2] =
	{
		{0, 0},
		{60, 0},
		{115, -52},
		{98, -55},
		{122, -60}};

static void __forceinline XA_decode_block(s16* buffer, const s16* block, s32& prev1, s32& prev2)
{
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

static void __forceinline IncrementNextA(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

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

static __forceinline s32 GetNextDataBuffered(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

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
		IncrementNextA(thiscore, voiceidx);

		if ((vc.NextA & 7) == 0) // vc.SCurrent == 24 equivalent
		{
			if (vc.LoopFlags & XAFLAG_LOOP_END)
			{
				thiscore.Regs.ENDX |= (1 << voiceidx);
				vc.NextA = vc.LoopStartA | 1;
				if (!(vc.LoopFlags & XAFLAG_LOOP))
					vc.Stop();
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

static __forceinline void GetNextDataDummy(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	IncrementNextA(thiscore, voiceidx);

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

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
//                                                                                     //

static __forceinline s32 ApplyVolume(s32 data, s32 volume)
{
	return (volume * data) >> 15;
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeLR& volume)
{
	StereoOut32 val;
	val.Left  = ApplyVolume(data.Left, volume.Left);
	val.Right = ApplyVolume(data.Right, volume.Right);
	return val;
}

static __forceinline StereoOut32 ApplyVolume(const StereoOut32& data, const V_VolumeSlideLR& volume)
{
	StereoOut32 val;
	val.Left  = ApplyVolume(data.Left, volume.Left.Value);
	val.Right = ApplyVolume(data.Right, volume.Right.Value);
	return val;
}

static void __forceinline UpdatePitch(uint coreidx, uint voiceidx)
{
	V_Voice& vc(Cores[coreidx].Voices[voiceidx]);
	s32 pitch;

	// [Air] : re-ordered comparisons: Modulated is much more likely to be zero than voice,
	//   and so the way it was before it's have to check both voice and modulated values
	//   most of the time.  Now it'll just check Modulated and short-circuit past the voice
	//   check (not that it amounts to much, but eh every little bit helps).
	if ((vc.Modulated == 0) || (voiceidx == 0))
		pitch = vc.Pitch;
	else
		pitch = std::clamp((vc.Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

	pitch = std::min(pitch, 0x3FFF);
	vc.SP += pitch;
}

static __forceinline void CalculateADSR(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	if (vc.ADSR.Phase == V_ADSR::PHASE_STOPPED)
	{
		vc.ADSR.Value = 0;
		return;
	}

	if (!vc.ADSR.Calculate(thiscore.Index | (voiceidx << 1)))
		vc.Stop();
}

__forceinline static s32 GaussianInterpolate(s32 pv4, s32 pv3, s32 pv2, s32 pv1, s32 i)
{
	s32 out = 0;
	out =  (interpTable[i][0] * pv4) >> 15;
	out += (interpTable[i][1] * pv3) >> 15;
	out += (interpTable[i][2] * pv2) >> 15;
	out += (interpTable[i][3] * pv1) >> 15;

	return out;
}

static __forceinline s32 GetVoiceValues(V_Core& thiscore, uint voiceidx)
{
	V_Voice& vc(thiscore.Voices[voiceidx]);

	while (vc.SP >= 0)
	{
		vc.PV4 = vc.PV3;
		vc.PV3 = vc.PV2;
		vc.PV2 = vc.PV1;
		vc.PV1 = GetNextDataBuffered(thiscore, voiceidx);
		vc.SP -= 0x1000;
	}

	const s32 mu = vc.SP + 0x1000;

	return GaussianInterpolate(vc.PV4, vc.PV3, vc.PV2, vc.PV1, (mu & 0x0ff0) >> 4);
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


static __forceinline StereoOut32 MixVoice(V_Core& thiscore, uint coreidx, uint voiceidx)
{
	StereoOut32 voiceOut;
	s32 Value      = 0;

	V_Voice& vc(thiscore.Voices[voiceidx]);

	// Most games don't use much volume slide effects.  So only call the UpdateVolume
	// methods when needed by checking the flag outside the method here...
	// (Note: Ys 6 : Ark of Nephistm uses these effects)

	vc.Volume.Update();

	// SPU2 Note: The spu2 continues to process voices for eternity, always, so we
	// have to run through all the motions of updating the voice regardless of it's
	// audible status.  Otherwise IRQs might not trigger and emulation might fail.

	UpdatePitch(coreidx, voiceidx);

	voiceOut.Left  = 0;
	voiceOut.Right = 0;

	if (vc.ADSR.Phase > V_ADSR::PHASE_STOPPED)
	{
		StereoOut32 val;
		if (vc.Noise)
			Value = (s16)thiscore.NoiseOut;
		else
			Value = GetVoiceValues(thiscore, voiceidx);

		// Update and Apply ADSR  (applies to normal and noise sources)

		CalculateADSR(thiscore, voiceidx);
		Value     = ApplyVolume(Value, vc.ADSR.Value);
		vc.OutX   = Value;

		val.Left  = Value;
		val.Right = Value;
		voiceOut  = ApplyVolume(val, vc.Volume);
	}
	else
	{
		while (vc.SP >= 0)
			GetNextDataDummy(thiscore, voiceidx); // Dummy is enough
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
		StereoOut32 VVal(MixVoice(thiscore, coreidx, voiceidx));

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
	MasterVol.Update();
	UpdateNoise(*this);


	// Saturate final result to standard 16 bit range.
	Voices.Dry = clamp_mix(inVoices.Dry);
	Voices.Wet = clamp_mix(inVoices.Wet);

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
	StereoOut32 tmp = ApplyVolume(RV, FxVol);

	// Mix Dry + Wet
	// (master volume is applied later to the result of both outputs added together).
	TD.Left  += tmp.Left;
	TD.Right += tmp.Right;
	return TD;
}

static StereoOut32 DCFilter(StereoOut32 input) {
	// A simple DC blocking high-pass filter
	// Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
	// The magic number 0x7f5c is ceil(INT16_MAX * 0.995)
	StereoOut32 output;
	output.Left = (input.Left - DCFilterIn.Left + clamp_mix((0x7f5c * DCFilterOut.Left) >> 15));
	output.Right = (input.Right - DCFilterIn.Right + clamp_mix((0x7f5c * DCFilterOut.Right) >> 15));

	DCFilterIn = input;
	DCFilterOut = output;
	return output;
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
	StereoOut32 tmp0 = ApplyVolume(Cores[0].ReadInput(), Cores[0].InpVol);
	StereoOut32 tmp1;

	empty.Left = empty.Right = 0;
	if (PlayMode & 8)
		tmp1 = empty;
	else
		tmp1 = ApplyVolume(Cores[1].ReadInput(), Cores[1].InpVol);

	InputData[0] = tmp0;
	InputData[1] = tmp1;

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
		Ext = ApplyVolume(clamp_mix(Ext), Cores[0].MasterVol);

	// Commit Core 0 output to ram before mixing Core 1:
	spu2M_WriteFast(0x800 + OutPos, Ext.Left);
	spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

	Ext = ApplyVolume(Ext, Cores[1].ExtVol);
	Out = Cores[1].Mix(VoiceData[1], InputData[1], Ext);

	// Experimental CDDA support
	// The CDDA overrides all other mixer output.  It's a direct feed!
	if (PlayMode & 8)
		Out = Cores[1].ReadInput_HiFi();
	else
		Out = ApplyVolume(clamp_mix(Out), Cores[1].MasterVol);

	Out = DCFilter(Out);

	// Final clamp, take care not to exceed 16 bits from here on
	Out = clamp_mix(Out);

	OutS16.Left  = (s16)Out.Left;
	OutS16.Right = (s16)Out.Right;

	SndBuffer::Write(OutS16);

	// Update AutoDMA output positioning
	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;
}
