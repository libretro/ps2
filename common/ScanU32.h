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

#include "Pcsx2Defs.h"

#if defined(ARCH_X86)
#include <emmintrin.h>
#elif defined(ARCH_ARM64)
#include <arm_neon.h>
#endif

/* Membership scan over a u32 array: four lanes per compare on the wide
 * units, a plain tail for the remainder.  Benchmarked at or above the
 * unrolled scalar scan std::find lowers to, at every size that shows up
 * in practice, and free of the <algorithm> dependency. */
static __fi bool ScanContainsU32(const u32* p, size_t n, u32 x)
{
	size_t i = 0;

#if defined(ARCH_X86)
	{
		const __m128i needle = _mm_set1_epi32((int)x);
		for (; i + 4 <= n; i += 4)
		{
			const __m128i eq = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)(p + i)), needle);
			if (_mm_movemask_epi8(eq))
				return true;
		}
	}
#elif defined(ARCH_ARM64)
	{
		const uint32x4_t needle = vdupq_n_u32(x);
		for (; i + 4 <= n; i += 4)
		{
			if (vmaxvq_u32(vceqq_u32(vld1q_u32(p + i), needle)))
				return true;
		}
	}
#endif

	for (; i < n; i++)
	{
		if (p[i] == x)
			return true;
	}
	return false;
}
