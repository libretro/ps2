/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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
#include "GS/GS.h"
#include "GS/GSExtra.h"
#include "GS/GSUtil.h"
#include "MultiISA.h"

#include "common/Console.h"

#ifdef ENABLE_VULKAN
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include "GS/Renderers/DX11/D3D.h"
#endif

static struct GSUtilMaps
{
public:
	u32 CompatibleBitsField[64][2];
	u32 SharedBitsField[64][2] =    {
						{0, 0}, /* 0   */
						{1, 4098}, /* 1   */
						{0, 0}, /* 2   */
						{0, 0}, /* 3   */
						{0, 0}, /* 4   */
						{0, 0}, /* 5   */
						{0, 0}, /* 6   */
						{0, 0}, /* 7   */
						{0, 0}, /* 8   */
						{0, 0}, /* 9   */
						{0, 0}, /* 10  */
						{0, 0}, /* 11  */
						{0, 0}, /* 12  */
						{0, 0}, /* 13  */
						{0, 0}, /* 14  */
						{0, 0}, /* 15  */
						{0, 0}, /* 16  */
						{0, 0}, /* 17  */
						{0, 0}, /* 18  */
						{0, 0}, /* 19  */
						{0, 0}, /* 20  */
						{0, 0}, /* 21  */
						{0, 0}, /* 22  */
						{0, 0}, /* 23  */
						{0, 0}, /* 24  */
						{0, 0}, /* 25  */
						{0, 0}, /* 26  */
						{2, 131072}, /* 27  */
						{0, 0}, /* 28  */
						{0, 0}, /* 29  */
						{0, 0}, /* 30  */
						{0, 0}, /* 31  */
						{0, 0}, /* 32  */
						{0, 0}, /* 33  */
						{0, 0}, /* 34  */
						{0, 0}, /* 35  */
						{2, 135168}, /* 36  */
						{0, 0}, /* 37  */
						{0, 0}, /* 38  */
						{0, 0}, /* 39  */
						{0, 0}, /* 40  */
						{0, 0}, /* 41  */
						{0, 0}, /* 42  */
						{0, 0}, /* 43  */
						{2, 131074}, /* 44  */
						{0, 0}, /* 45  */
						{0, 0}, /* 46  */
						{0, 0}, /* 47  */
						{0, 0}, /* 48  */
						{134217728, 4112}, /* 49  */
						{0, 0}, /* 50  */
						{0, 0}, /* 51  */
						{0, 0}, /* 52  */
						{0, 0}, /* 53  */
						{0, 0}, /* 54  */
						{0, 0}, /* 55  */
						{0, 0}, /* 56  */
						{0, 0}, /* 57  */
						{0, 0}, /* 58  */
						{0, 0}, /* 59  */
						{0, 0}, /* 60  */
						{0, 0}, /* 61  */
						{0, 0}, /* 62  */
						{0, 0}, /* 63  */
					};
 	u32 SwizzleField[64][2];
} s_maps;

void GSUtil::Init()
{
	memset(s_maps.CompatibleBitsField, 0, sizeof(s_maps.CompatibleBitsField));
	memset(s_maps.SwizzleField, 0, sizeof(s_maps.SwizzleField));

	for (int i = 0; i < 64; i++)
	{
		s_maps.CompatibleBitsField[i][i >> 5] |= 1U << (i & 0x1f);
		s_maps.SwizzleField       [i][i >> 5] |= 1U << (i & 0x1f);
	}

	s_maps.CompatibleBitsField[PSMCT32][PSMCT24 >> 5]  |= 1 << (PSMCT24 & 0x1f);
	s_maps.CompatibleBitsField[PSMCT24][PSMCT32 >> 5]  |= 1 << (PSMCT32 & 0x1f);
	s_maps.CompatibleBitsField[PSMCT16][PSMCT16S >> 5] |= 1 << (PSMCT16S & 0x1f);
	s_maps.CompatibleBitsField[PSMCT16S][PSMCT16 >> 5] |= 1 << (PSMCT16 & 0x1f);
	s_maps.CompatibleBitsField[PSMZ32][PSMZ24 >> 5]    |= 1 << (PSMZ24 & 0x1f);
	s_maps.CompatibleBitsField[PSMZ24][PSMZ32 >> 5]    |= 1 << (PSMZ32 & 0x1f);
	s_maps.CompatibleBitsField[PSMZ16][PSMZ16S >> 5]   |= 1 << (PSMZ16S & 0x1f);
	s_maps.CompatibleBitsField[PSMZ16S][PSMZ16 >> 5]   |= 1 << (PSMZ16 & 0x1f);

	s_maps.SwizzleField[PSMCT32][PSMCT24 >> 5]         |= 1 << (PSMCT24 & 0x1f);
	s_maps.SwizzleField[PSMCT24][PSMCT32 >> 5]         |= 1 << (PSMCT32 & 0x1f);
	s_maps.SwizzleField[PSMT8H][PSMCT32 >> 5]          |= 1 << (PSMCT32 & 0x1f);
	s_maps.SwizzleField[PSMCT32][PSMT8H >> 5]          |= 1 << (PSMT8H & 0x1f);
	s_maps.SwizzleField[PSMT4HL][PSMCT32 >> 5]         |= 1 << (PSMCT32 & 0x1f);
	s_maps.SwizzleField[PSMCT32][PSMT4HL >> 5]         |= 1 << (PSMT4HL & 0x1f);
	s_maps.SwizzleField[PSMT4HH][PSMCT32 >> 5]         |= 1 << (PSMCT32 & 0x1f);
	s_maps.SwizzleField[PSMCT32][PSMT4HH >> 5]         |= 1 << (PSMT4HH & 0x1f);
	s_maps.SwizzleField[PSMZ32][PSMZ24 >> 5]           |= 1 << (PSMZ24 & 0x1f);
	s_maps.SwizzleField[PSMZ24][PSMZ32 >> 5]           |= 1 << (PSMZ32 & 0x1f);
}

GS_PRIM_CLASS GSUtil::GetPrimClass(u32 prim)
{
	static u8 PrimClassField[8] = {	GS_POINT_CLASS, 
					GS_LINE_CLASS,
					GS_LINE_CLASS,
					GS_TRIANGLE_CLASS,
					GS_TRIANGLE_CLASS,
					GS_TRIANGLE_CLASS,
					GS_SPRITE_CLASS,
					GS_INVALID_CLASS
					};
	return (GS_PRIM_CLASS)PrimClassField[prim];
}

const u32* GSUtil::HasSharedBitsPtr(u32 dpsm)
{
	return s_maps.SharedBitsField[dpsm];
}

bool GSUtil::HasSharedBits(u32 spsm, const u32* RESTRICT ptr)
{
	return (ptr[spsm >> 5] & (1 << (spsm & 0x1f))) == 0;
}

// Pixels can NOT coexist in the same 32bits of space.
// Example: Using PSMT8H or PSMT4HL/HH with CT24 would fail this check.
bool GSUtil::HasSharedBits(u32 spsm, u32 dpsm)
{
	return (s_maps.SharedBitsField[dpsm][spsm >> 5] & (1 << (spsm & 0x1f))) == 0;
}

// Pixels can NOT coexist in the same 32bits of space.
// Example: Using PSMT8H or PSMT4HL/HH with CT24 would fail this check.
// SBP and DBO must match.
bool GSUtil::HasSharedBits(u32 sbp, u32 spsm, u32 dbp, u32 dpsm)
{
	return ((sbp ^ dbp) | (s_maps.SharedBitsField[dpsm][spsm >> 5] & (1 << (spsm & 0x1f)))) == 0;
}

// Shares bit depths, only detects 16/24/32 bit formats.
// 24/32bit cross compatible, 16bit compatbile with 16bit.
bool GSUtil::HasCompatibleBits(u32 spsm, u32 dpsm)
{
	return (s_maps.CompatibleBitsField[spsm][dpsm >> 5] & (1 << (dpsm & 0x1f))) != 0;
}

bool GSUtil::HasSameSwizzleBits(u32 spsm, u32 dpsm)
{
	return (s_maps.SwizzleField[spsm][dpsm >> 5] & (1 << (dpsm & 0x1f))) != 0;
}

u32 GSUtil::GetChannelMask(u32 spsm)
{
	switch (spsm)
	{
		case PSMCT24:
		case PSMZ24:
			return 0x7;
		case PSMT8H:
		case PSMT4HH: // This sucks, I'm sorry, but we don't have a way to do half channels
		case PSMT4HL: // So uuhh TODO I guess.
			return 0x8;
		default:
			break;
	}
	return 0xf;
}

u32 GSUtil::GetChannelMask(u32 spsm, u32 fbmsk)
{
	u32 mask = GetChannelMask(spsm);
	mask &= ((fbmsk & 0xFF) == 0xFF) ? (~0x1 & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF00) == 0xFF00) ? (~0x2 & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF0000) == 0xFF0000) ? (~0x4 & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF000000) == 0xFF000000) ? (~0x8 & 0xf) : 0xf;
	return mask;
}

GSRendererType GSUtil::GetPreferredRenderer()
{
#if defined(_WIN32)
	// Use D3D device info to select renderer.
	return D3D::GetPreferredRenderer();
#else
	// Linux: Prefer Vulkan if the driver isn't buggy.
#if defined(ENABLE_VULKAN)
	if (GSDeviceVK::IsSuitableDefaultRenderer())
		return GSRendererType::VK;
#endif

	// Otherwise, whatever is available.
#if defined(ENABLE_OPENGL)
	return GSRendererType::OGL;
#elif defined(ENABLE_VULKAN)
	return GSRendererType::VK;
#else
	return GSRendererType::SW;
#endif
#endif
}
