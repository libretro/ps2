/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include "common/PrecompiledHeader.h"

#include "D3D12Util.h"
#include "common/StringUtil.h"

void D3D12::SetDefaultSampler(D3D12_SAMPLER_DESC* desc)
{
	desc->Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	desc->AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc->AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc->AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	desc->MipLODBias = 0;
	desc->MaxAnisotropy = 1;
	desc->ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	desc->BorderColor[0] = 1.0f;
	desc->BorderColor[1] = 1.0f;
	desc->BorderColor[2] = 1.0f;
	desc->BorderColor[3] = 1.0f;
	desc->MinLOD = -3.402823466e+38F; // -FLT_MAX
	desc->MaxLOD = 3.402823466e+38F; // FLT_MAX
}
