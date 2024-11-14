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

static constexpr s32 ADSR_MAX_VOL = 0x7fff;

void ADSR_UpdateCache(V_ADSR &v)
{
	v.CachedPhases[PHASE_ATTACK].Decr   = false;
	v.CachedPhases[PHASE_ATTACK].Exp    = v.AttackMode;
	v.CachedPhases[PHASE_ATTACK].Shift  = v.AttackShift;
	v.CachedPhases[PHASE_ATTACK].Step   = 7 - v.AttackStep;
	v.CachedPhases[PHASE_ATTACK].Target = ADSR_MAX_VOL;

	v.CachedPhases[PHASE_DECAY].Decr    = true;
	v.CachedPhases[PHASE_DECAY].Exp     = true;
	v.CachedPhases[PHASE_DECAY].Shift   = v.DecayShift;
	v.CachedPhases[PHASE_DECAY].Step    = -8;
	v.CachedPhases[PHASE_DECAY].Target  = (v.SustainLevel + 1) << 11;

	v.CachedPhases[PHASE_SUSTAIN].Decr  = v.SustainDir;
	v.CachedPhases[PHASE_SUSTAIN].Exp   = v.SustainMode;
	v.CachedPhases[PHASE_SUSTAIN].Shift = v.SustainShift;
	v.CachedPhases[PHASE_SUSTAIN].Step  = 7 - v.SustainStep;

	if (v.CachedPhases[PHASE_SUSTAIN].Decr)
		v.CachedPhases[PHASE_SUSTAIN].Step = ~v.CachedPhases[PHASE_SUSTAIN].Step;

	v.CachedPhases[PHASE_SUSTAIN].Target = 0;

	v.CachedPhases[PHASE_RELEASE].Decr   = true;
	v.CachedPhases[PHASE_RELEASE].Exp    = v. ReleaseMode;
	v.CachedPhases[PHASE_RELEASE].Shift  = v.ReleaseShift;
	v.CachedPhases[PHASE_RELEASE].Step   = -8;
	v.CachedPhases[PHASE_RELEASE].Target = 0;
}

bool ADSR_Calculate(V_ADSR &v, int voiceidx)
{
	auto& p = v.CachedPhases.at(v.Phase);

	// maybe not correct for the "infinite" settings
	u32 counter_inc = 0x8000 >> std::max(0, p.Shift - 11);
	s32 level_inc   = p.Step << std::max(0, 11 - p.Shift);

	if (p.Exp)
	{
		if (p.Decr)
			level_inc = (s16)((level_inc * v.Value) >> 15);
		else
		{
			if (v.Value > 0x6000)
				counter_inc >>= 2;
		}
	}

	counter_inc = std::max<u32>(1, counter_inc);
	v.Counter  += counter_inc;

	if (v.Counter >= 0x8000)
	{
		v.Counter = 0;
		v.Value   = std::clamp<s32>(v.Value + level_inc, 0, INT16_MAX);
	}

	// Stay in sustain until key off or silence
	if (v.Phase == PHASE_SUSTAIN)
		return v.Value != 0;

	// Check if target is reached to advance phase
	if ((!p.Decr && v.Value >= p.Target) || (p.Decr && v.Value <= p.Target))
		v.Phase++;

	// All phases done, stop the voice
	if (v.Phase > PHASE_RELEASE)
		return false;

	return true;
}

void ADSR_Release(V_ADSR &v)
{
	if (v.Phase != PHASE_STOPPED)
	{
		v.Phase   = PHASE_RELEASE;
		v.Counter = 0;
	}
}
