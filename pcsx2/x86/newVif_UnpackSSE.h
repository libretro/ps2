/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include "../Common.h"
#include "../Vif_Dma.h"
#include "newVif.h"

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Base
// --------------------------------------------------------------------------------------
class VifUnpackSSE_Base
{
public:
	bool usn;    // unsigned flag
	bool doMask; // masking write enable flag
	int  UnpkLoopIteration;
	int  IsAligned;

protected:
	xAddressVoid dstIndirect;
	xAddressVoid srcIndirect;
	xRegisterSSE zeroReg;
	xRegisterSSE workReg;
	xRegisterSSE destReg;

public:
	VifUnpackSSE_Base();
	virtual ~VifUnpackSSE_Base() = default;

	virtual void xUnpack(int upktype) const;
	virtual bool IsWriteProtectedOp() const = 0;
	virtual bool IsInputMasked() const = 0;
	virtual bool IsUnmaskedOp() const = 0;
	virtual void xMovDest() const;

protected:
	virtual void doMaskWrite(const xRegisterSSE& regX) const = 0;

	virtual void xPMOVXX8(const xRegisterSSE& regX) const;
	virtual void xPMOVXX16(const xRegisterSSE& regX) const;

	virtual void xUPK_S_32() const;
	virtual void xUPK_S_16() const;
	virtual void xUPK_S_8() const;

	virtual void xUPK_V2_32() const;
	virtual void xUPK_V2_16() const;
	virtual void xUPK_V2_8() const;

	virtual void xUPK_V3_32() const;
	virtual void xUPK_V3_16() const;
	virtual void xUPK_V3_8() const;

	virtual void xUPK_V4_32() const;
	virtual void xUPK_V4_16() const;
	virtual void xUPK_V4_8() const;
	virtual void xUPK_V4_5() const;
};

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Simple
// --------------------------------------------------------------------------------------
class VifUnpackSSE_Simple : public VifUnpackSSE_Base
{
	typedef VifUnpackSSE_Base _parent;

public:
	int curCycle;

public:
	VifUnpackSSE_Simple() = default;
	virtual ~VifUnpackSSE_Simple() = default;

	virtual bool IsWriteProtectedOp() const { return false; }
	virtual bool IsInputMasked() const { return false; }
	virtual bool IsUnmaskedOp() const { return !doMask; }

protected:
	virtual void doMaskWrite(const xRegisterSSE& regX) const;
};

// --------------------------------------------------------------------------------------
//  VifUnpackSSE_Dynarec
// --------------------------------------------------------------------------------------
class VifUnpackSSE_Dynarec : public VifUnpackSSE_Base
{
	typedef VifUnpackSSE_Base _parent;

public:
	bool isFill;
	int  doMode; // two bit value representing difference mode
	bool skipProcessing;
	bool inputMasked;

protected:
	const nVifStruct& v;   // vif0 or vif1
	const nVifBlock&  vB;  // some pre-collected data from VifStruct
	int               vCL; // internal copy of vif->cl

public:
	VifUnpackSSE_Dynarec(const nVifStruct& vif_, const nVifBlock& vifBlock_);
	VifUnpackSSE_Dynarec(const VifUnpackSSE_Dynarec& src) // copy constructor
		: _parent(src)
		, v(src.v)
		, vB(src.vB)
	{
		isFill = src.isFill;
		vCL    = src.vCL;
	}

	virtual ~VifUnpackSSE_Dynarec() = default;

	virtual bool IsWriteProtectedOp() const { return skipProcessing; }
	virtual bool IsInputMasked() const { return inputMasked; }
	virtual bool IsUnmaskedOp() const { return !doMode && !doMask; }

	void ModUnpack(int upknum, bool PostOp);
	void ProcessMasks();
	void CompileRoutine();


protected:
	virtual void doMaskWrite(const xRegisterSSE& regX) const;
	void SetMasks(int cS) const;
};
