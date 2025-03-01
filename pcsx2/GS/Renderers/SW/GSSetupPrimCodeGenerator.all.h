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

#pragma once

#include "GSScanlineEnvironment.h"
#include "GSNewCodeGenerator.h"
#include "../../MultiISA.h"

#if _M_SSE >= 0x501
	#define SETUP_PRIM_VECTOR_REGISTER Xbyak::Ymm
	#define SETUP_PRIM_USING_XMM 0
	#define SETUP_PRIM_USING_YMM 1
#else
	#define SETUP_PRIM_VECTOR_REGISTER Xbyak::Xmm
	#define SETUP_PRIM_USING_XMM 1
	#define SETUP_PRIM_USING_YMM 0
#endif

MULTI_ISA_UNSHARED_START

class GSSetupPrimCodeGenerator2 : public GSNewCodeGenerator
{
	using _parent = GSNewCodeGenerator;
	using XYm = SETUP_PRIM_VECTOR_REGISTER;

	using Xmm = Xbyak::Xmm;
	using Ymm = Xbyak::Ymm;

	constexpr static bool isXmm = std::is_same<XYm, Xbyak::Xmm>::value;
	constexpr static bool isYmm = std::is_same<XYm, Xbyak::Ymm>::value;
	constexpr static int vecsize = isXmm ? 16 : 32;

	constexpr static int dsize = isXmm ? 4 : 8;

	GSScanlineSelector m_sel;
	bool many_regs;

	struct {u32 z:1, f:1, t:1, c:1;} m_en;

	const XYm xym0{0}, xym1{1}, xym2{2}, xym3{3}, xym4{4}, xym5{5}, xym6{6}, xym7{7}, xym8{8}, xym9{9}, xym10{10}, xym11{11}, xym12{12}, xym13{13}, xym14{14}, xym15{15};
	const AddressReg _64_vertex, _index, _dscan, _m_local, t1;

public:
	GSSetupPrimCodeGenerator2(Xbyak::CodeGenerator* base, u64 key);
	void Generate();

private:
	/// Broadcast 128 bits of floats from memory to the whole register, whatever size that register might be
	void broadcastf128(const XYm& reg, const Xbyak::Address& mem);
	/// Broadcast a 32-bit float to the whole register, whatever size that register might be
	void broadcastss(const XYm& reg, const Xbyak::Address& mem);

	void Depth_XMM();
	void Depth_YMM();
	void Texture();
	void Color();
};

MULTI_ISA_UNSHARED_END
