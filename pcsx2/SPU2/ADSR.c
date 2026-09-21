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

#include <string.h>

#include "Global.h"

#define ADSR_MAX_VOL 0x7fff

void ADSR_UpdateCache(V_ADSR *v)
{
	v->CachedPhases[PHASE_ATTACK].Decr   = false;
	v->CachedPhases[PHASE_ATTACK].Exp    = v->AttackMode;
	v->CachedPhases[PHASE_ATTACK].Shift  = v->AttackShift;
	v->CachedPhases[PHASE_ATTACK].Step   = 7 - v->AttackStep;
	v->CachedPhases[PHASE_ATTACK].Target = ADSR_MAX_VOL;

	v->CachedPhases[PHASE_DECAY].Decr    = true;
	v->CachedPhases[PHASE_DECAY].Exp     = true;
	v->CachedPhases[PHASE_DECAY].Shift   = v->DecayShift;
	v->CachedPhases[PHASE_DECAY].Step    = -8;
	v->CachedPhases[PHASE_DECAY].Target  = (v->SustainLevel + 1) << 11;

	v->CachedPhases[PHASE_SUSTAIN].Decr  = v->SustainDir;
	v->CachedPhases[PHASE_SUSTAIN].Exp   = v->SustainMode;
	v->CachedPhases[PHASE_SUSTAIN].Shift = v->SustainShift;
	v->CachedPhases[PHASE_SUSTAIN].Step  = 7 - v->SustainStep;

	if (v->CachedPhases[PHASE_SUSTAIN].Decr)
		v->CachedPhases[PHASE_SUSTAIN].Step = ~v->CachedPhases[PHASE_SUSTAIN].Step;

	v->CachedPhases[PHASE_SUSTAIN].Target = 0;

	v->CachedPhases[PHASE_RELEASE].Decr   = true;
	v->CachedPhases[PHASE_RELEASE].Exp    = v->ReleaseMode;
	v->CachedPhases[PHASE_RELEASE].Shift  = v->ReleaseShift;
	v->CachedPhases[PHASE_RELEASE].Step   = -8;
	v->CachedPhases[PHASE_RELEASE].Target = 0;
}
