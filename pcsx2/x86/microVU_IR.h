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
#include "common/emitter/c89ops.h"
#include "microVU.h"

#include "../microVU/microVU_Types.h"

//------------------------------------------------------------------
// Reg Alloc
//------------------------------------------------------------------

class microRegAlloc
{
protected:
	static const int xmmTotal = iREGCNT_XMM - 1; // PQ register is reserved
	static const int gprTotal = iREGCNT_GPR;

	microMapXMM xmmMap[xmmTotal];
	microMapGPR gprMap[gprTotal];

	int         counter; // Current allocation count
	int         index;   // VU0 or VU1

	// DO NOT REMOVE THIS.
	// This is here for a reason. MSVC likes to turn global writes into a load+conditional move+store.
	// That creates a race with the EE thread when we're compiling on the VU thread, even though
	// regAllocCOP2 is false. By adding another level of indirection, it emits a branch instead.
	_xmmregs*   pxmmregs;

	bool        regAllocCOP2;    // Local COP2 check

	// Helper functions to get VU regs
	VURegs& regs() const { return ::vuRegs[index]; }

	__ri void loadIreg(const xmm& reg, int xyzw)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == REG_I)
			{
				xe_movdzx_xr(reg.Id, i);
				if (!_XYZWss(xyzw))
					xe_shufps_xxi(reg.Id, reg.Id, 0);

				return;
			}
		}

		xe_movss_xm(reg.Id, &::vuRegs[index].VI[REG_I]);
		if (!_XYZWss(xyzw))
			xe_shufps_xxi(reg.Id, reg.Id, 0);
	}

	int findFreeRegRec(int startIdx)
	{
		for (int i = startIdx; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded)
			{
				int x = findFreeRegRec(i + 1);
				if (x == -1)
					return i;
				return ((xmmMap[i].count < xmmMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeReg(int vfreg)
	{
		if (regAllocCOP2)
		{
			return _allocVFtoXMMreg(vfreg, 0);
		}

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		return findFreeRegRec(0);
	}

	int findFreeGPRRec(int startIdx)
	{
		for (int i = startIdx; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded)
			{
				int x = findFreeGPRRec(i + 1);
				if (x == -1)
					return i;
				return ((gprMap[i].count < gprMap[x].count) ? i : x);
			}
		}
		return -1;
	}

	int findFreeGPR(int vireg)
	{
		if (regAllocCOP2)
			return _allocX86reg(X86TYPE_VIREG, vireg, MODE_COP2);

		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].usable && !gprMap[i].isNeeded && (gprMap[i].VIreg < 0))
			{
				return i; // Reg is not needed and was a temp reg
			}
		}
		return findFreeGPRRec(0);
	}

	void writeVIBackup(const xRegisterInt& reg);

public:
	microRegAlloc(int _index)
	{
		int i;
		index = _index;

		// mark GPR registers as usable
		for (i = 0; i < gprTotal; i++)
		{
			gprMap[i].VIreg          = 0;
			gprMap[i].count          = 0;
			gprMap[i].isNeeded       = false;
			gprMap[i].dirty          = false;
			gprMap[i].isZeroExtended = false;
			gprMap[i].usable         = false;

			if (i == gprT1.Id || i == gprT2.Id ||
				i == gprF0.Id || i == gprF1.Id || i == gprF2.Id || i == gprF3.Id ||
				i == rsp.Id)
				continue;

			gprMap[i].usable = true;
		}

		reset(false);
	}

	// Fully resets the regalloc by clearing all cached data
	void reset(bool cop2mode)
	{
		// we run this at the of cop2, so don't free fprs
		regAllocCOP2 = false;

		for (int i = 0; i < xmmTotal; i++)
			clearReg(i);
		for (int i = 0; i < gprTotal; i++)
			clearGPR(i);

		counter = 0;
		regAllocCOP2 = cop2mode;
		pxmmregs = cop2mode ? xmmregs : nullptr;

		if (cop2mode)
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				if (!pxmmregs[i].inuse || pxmmregs[i].type != XMMTYPE_VFREG)
					continue;

				// we shouldn't have any temp registers in here.. except for PQ, which
				// isn't allocated here yet.
				if (pxmmregs[i].reg >= 0)
				{
					pxmmregs[i].needed = false;
					xmmMap[i].isNeeded = false;
					xmmMap[i].VFreg = pxmmregs[i].reg;
					xmmMap[i].xyzw = ((pxmmregs[i].mode & MODE_WRITE) != 0) ? 0xf : 0x0;
				}
			}

			for (int i = 0; i < gprTotal; i++)
			{
				if (!x86regs[i].inuse || x86regs[i].type != X86TYPE_VIREG)
					continue;

				if (x86regs[i].reg >= 0)
				{
					x86regs[i].needed = false;
					gprMap[i].isNeeded = false;
					gprMap[i].isZeroExtended = false;
					gprMap[i].VIreg = x86regs[i].reg;
					gprMap[i].dirty = ((x86regs[i].mode & MODE_WRITE) != 0);
				}
			}
		}

		gprMap[RFASTMEMBASE.Id].usable = !cop2mode || !CHECK_FASTMEM;
	}

	int getXmmCount()
	{
		return xmmTotal + 1;
	}

	int getFreeXmmCount()
	{
		int count = 0;

		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xmmMap[i].isNeeded && (xmmMap[i].VFreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVF(int vfreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == vfreg)
				return true;
		}

		return false;
	}

	int getRegVF(int i)
	{
		return (i < xmmTotal) ? xmmMap[i].VFreg : -1;
	}

	int getGPRCount()
	{
		return gprTotal;
	}

	int getFreeGPRCount()
	{
		int count = 0;

		for (int i = 0; i < gprTotal; i++)
		{
			if (!gprMap[i].usable && (gprMap[i].VIreg < 0))
				count++;
		}

		return count;
	}

	bool hasRegVI(int vireg)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			if (gprMap[i].VIreg == vireg)
				return true;
		}

		return false;
	}

	int getRegVI(int i)
	{
		return (i < gprTotal) ? gprMap[i].VIreg : -1;
	}

	// Flushes all allocated registers (i.e. writes-back to memory all modified registers).
	// If clearState is 0, then it keeps cached reg data valid
	// If clearState is 1, then it invalidates all cached reg data after write-back
	void flushAll(bool clearState = true)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			writeBackReg(xmm(i));
			if (clearState)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			writeBackReg(xRegister32(i), true);
			if (clearState)
				clearGPR(i);
		}
	}

	void flushCallerSavedRegisters(bool clearNeeded = false)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (!xRegisterSSE::IsCallerSaved(i))
				continue;

			writeBackReg(xmm(i));
			if (clearNeeded || !xmmMap[i].isNeeded)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			if (!xRegister32::IsCallerSaved(i))
				continue;

			writeBackReg(xRegister32(i), true);
			if (clearNeeded || !gprMap[i].isNeeded)
				clearGPR(i);
		}
	}

	void flushPartialForCOP2()
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& clear = xmmMap[i];

			// toss away anything which is not a full cached register
			if (pxmmregs[i].inuse && pxmmregs[i].type == XMMTYPE_VFREG)
			{
				// Should've been done in clearNeeded()
				if (clear.xyzw != 0 && clear.xyzw != 0xf)
					writeBackReg(xRegisterSSE::GetInstance(i), false);

				if (clear.VFreg <= 0)
				{
					// temps really shouldn't be here..
					_freeXMMreg(i);
				}
			}

			// needed gets cleared in iCore.
			clear = {-1, 0, 0, false, false};
		}

		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& clear = gprMap[i];
			if (clear.VIreg < 0)
				clearGPR(i);
		}
	}

	void TDwritebackAll()
	{
		// NOTE: We don't clear state here, this happens in an optional branch

		for (int i = 0; i < xmmTotal; i++)
		{
			microMapXMM& mapX = xmmMap[xmm(i).Id];

			if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
			{
				if (mapX.VFreg == 33)
					xe_movss_mx(&::vuRegs[index].VI[REG_I], i);
				else if (mapX.VFreg == 32)
					mVUsaveReg(xmm(i), e_mem_abs(&::vuRegs[index].ACC), mapX.xyzw, 1);
				else
					mVUsaveReg(xmm(i), e_mem_abs(&::vuRegs[index].VI[mapX.VFreg]), mapX.xyzw, 1);
			}
		}

		for (int i = 0; i < gprTotal; i++)
			writeBackReg(xRegister32(i), false);
	}

	bool checkVFClamp(int regId)
	{
		if (regId != xmmPQ.Id && ((xmmMap[regId].VFreg == 33 && !EmuConfig.Gamefixes.IbitHack) || xmmMap[regId].isZero))
			return false;
		else
			return true;
	}

	bool checkCachedReg(int regId)
	{
		if (regId < xmmTotal)
			return xmmMap[regId].VFreg >= 0;
		else
			return false;
	}

	bool checkCachedGPR(int regId)
	{
		if (regId < gprTotal)
			return gprMap[regId].VIreg >= 0 || gprMap[regId].isNeeded;
		else
			return false;
	}

	void clearReg(const xmm& reg) { clearReg(reg.Id); }
	void clearReg(int regId)
	{
		microMapXMM& clear = xmmMap[regId];
		if (regAllocCOP2 && (clear.isNeeded || clear.VFreg >= 0))
			pxmmregs[regId].inuse = false;

		clear = {-1, 0, 0, false, false};
	}

	void clearRegVF(int VFreg)
	{
		for (int i = 0; i < xmmTotal; i++)
		{
			if (xmmMap[i].VFreg == VFreg)
				clearReg(i);
		}
	}

	void clearRegCOP2(int xmmReg)
	{
		if (regAllocCOP2)
			clearReg(xmmReg);
	}

	void updateCOP2AllocState(int rn)
	{
		if (!regAllocCOP2)
			return;

		const bool dirty = (xmmMap[rn].VFreg > 0 && xmmMap[rn].xyzw != 0);
		pxmmregs[rn].reg = xmmMap[rn].VFreg;
		pxmmregs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		pxmmregs[rn].needed = xmmMap[rn].isNeeded;
	}

	// Writes back modified reg to memory.
	// If all vectors modified, then keeps the VF reg cached in the xmm register.
	// If reg was not modified, then keeps the VF reg cached in the xmm register.
	void writeBackReg(const xmm& reg, bool invalidateRegs = true)
	{
		microMapXMM& mapX = xmmMap[reg.Id];

		if ((mapX.VFreg > 0) && mapX.xyzw) // Reg was modified and not Temp or vf0
		{
			if (mapX.VFreg == 33)
				xe_movss_mx(&::vuRegs[index].VI[REG_I], reg.Id);
			else if (mapX.VFreg == 32)
				mVUsaveReg(reg, e_mem_abs(&::vuRegs[index].ACC), mapX.xyzw, true);
			else
				mVUsaveReg(reg, e_mem_abs(&::vuRegs[index].VF[mapX.VFreg]), mapX.xyzw, true);

			if (invalidateRegs)
			{
				for (int i = 0; i < xmmTotal; i++)
				{
					microMapXMM& mapI = xmmMap[i];

					if ((i == reg.Id) || mapI.isNeeded)
						continue;

					if (mapI.VFreg == mapX.VFreg)
					{
						clearReg(i); // Invalidate any Cached Regs of same vf Reg
					}
				}
			}
			if (mapX.xyzw == 0xf) // Make Cached Reg if All Vectors were Modified
			{
				mapX.count    = counter;
				mapX.xyzw     = 0;
				mapX.isNeeded = false;
				updateCOP2AllocState(reg.Id);
				return;
			}
			clearReg(reg);
		}
		else if (mapX.xyzw) // Clear reg if modified and is VF0 or temp reg...
		{
			clearReg(reg);
		}
	}

	// Use this when done using the allocated register, it clears its "Needed" status.
	// The register that was written to, should be cleared before other registers are cleared.
	// This is to guarantee proper merging between registers... When a written-to reg is cleared,
	// it invalidates other cached registers of the same VF reg, and merges partial-vector
	// writes into them.
	void clearNeeded(const xmm& reg)
	{

		if ((reg.Id < 0) || (reg.Id >= xmmTotal)) // Sometimes xmmPQ hits this
			return;

		microMapXMM& clear = xmmMap[reg.Id];
		clear.isNeeded = false;
		if (clear.xyzw) // Reg was modified
		{
			if (clear.VFreg > 0)
			{
				int mergeRegs = 0;
				if (clear.xyzw < 0xf) // Try to merge partial writes
					mergeRegs = 1;
				for (int i = 0; i < xmmTotal; i++) // Invalidate any other read-only regs of same vfReg
				{
					if (i == reg.Id)
						continue;
					microMapXMM& mapI = xmmMap[i];
					if (mapI.VFreg == clear.VFreg)
					{
						if (mergeRegs == 1)
						{
							mVUmergeRegs(xmm(i), reg, clear.xyzw, true);
							mapI.xyzw  = 0xf;
							mapI.count = counter;
							mergeRegs  = 2;
							updateCOP2AllocState(i);
						}
						else
							clearReg(i); // Clears when mergeRegs is 0 or 2
					}
				}
				if (mergeRegs == 2) // Clear Current Reg if Merged
					clearReg(reg);
				else if (mergeRegs == 1) // Write Back Partial Writes if couldn't merge
					writeBackReg(reg);
			}
			else
				clearReg(reg); // If Reg was temp or vf0, then invalidate itself
		}
		else if (regAllocCOP2 && clear.VFreg < 0)
		{
			// free on the EE side
			pxmmregs[reg.Id].inuse = false;
		}
	}

	// vfLoadReg  = VF reg to be loaded to the xmm register
	// vfWriteReg = VF reg that the returned xmm register will be considered as
	// xyzw       = XYZW vectors that will be modified (and loaded)
	// cloneWrite = When loading a reg that will be written to, it copies it to its own xmm reg instead of overwriting the cached one...
	// Notes:
	// To load a temp reg use the default param values, vfLoadReg = -1 and vfWriteReg = -1.
	// To load a full reg which won't be modified and you want cached, specify vfLoadReg >= 0 and vfWriteReg = -1
	// To load a reg which you don't want written back or cached, specify vfLoadReg >= 0 and vfWriteReg = 0
	const xmm& allocReg(int vfLoadReg = -1, int vfWriteReg = -1, int xyzw = 0, bool cloneWrite = true)
	{
		counter++;
		if (vfLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < xmmTotal; i++)
			{
				const xmm& xmmI = xmm::GetInstance(i);
				microMapXMM& mapI = xmmMap[i];
				if ((mapI.VFreg == vfLoadReg)
				 && (!mapI.xyzw                           // Reg Was Not Modified
				  || (mapI.VFreg && (mapI.xyzw == 0xf)))) // Reg Had All Vectors Modified and != VF0
				{
					int z = i;
					if (vfWriteReg >= 0) // Reg will be modified
					{
						if (cloneWrite) // Clone Reg so as not to use the same Cached Reg
						{
							z = findFreeReg(vfWriteReg);
							const xmm& xmmZ = xmm::GetInstance(z);
							writeBackReg(xmmZ);

							if (xyzw == 4)
								xe_pshufd_xxi(xmmZ.Id, xmmI.Id, 1);
							else if (xyzw == 2)
								xe_pshufd_xxi(xmmZ.Id, xmmI.Id, 2);
							else if (xyzw == 1)
								xe_pshufd_xxi(xmmZ.Id, xmmI.Id, 3);
							else if (z != i)
								xe_movaps_xx(xmmZ.Id, xmmI.Id);

							mapI.count = counter; // Reg i was used, so update counter
						}
						else // Don't clone reg, but shuffle to adjust for SS ops
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								writeBackReg(xmmI);

							if (xyzw == 4)
								xe_pshufd_xxi(xmmI.Id, xmmI.Id, 1);
							else if (xyzw == 2)
								xe_pshufd_xxi(xmmI.Id, xmmI.Id, 2);
							else if (xyzw == 1)
								xe_pshufd_xxi(xmmI.Id, xmmI.Id, 3);
						}
						xmmMap[z].VFreg = vfWriteReg;
						xmmMap[z].xyzw = xyzw;
						xmmMap[z].isZero = (vfLoadReg == 0);
					}
					xmmMap[z].count = counter;
					xmmMap[z].isNeeded = true;
					updateCOP2AllocState(z);

					return xmm::GetInstance(z);
				}
			}
		}
		int x = findFreeReg((vfWriteReg >= 0) ? vfWriteReg : vfLoadReg);
		const xmm& xmmX = xmm::GetInstance(x);
		writeBackReg(xmmX);

		if (vfWriteReg >= 0) // Reg Will Be Modified (allow partial reg loading)
		{
			if ((vfLoadReg == 0) && !(xyzw & 1))
				xe_pxor_xx(xmmX.Id, xmmX.Id);
			else if (vfLoadReg == 33)
				loadIreg(xmmX, xyzw);
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, e_mem_abs(&::vuRegs[index].ACC), xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, e_mem_abs(&::vuRegs[index].VF[vfLoadReg]), xyzw);

			xmmMap[x].VFreg = vfWriteReg;
			xmmMap[x].xyzw  = xyzw;
		}
		else // Reg Will Not Be Modified (always load full reg for caching)
		{
			if (vfLoadReg == 33)
				loadIreg(xmmX, 0xf);
			else if (vfLoadReg == 32)
				xe_movaps_xm(xmmX.Id, &::vuRegs[index].ACC);
			else if (vfLoadReg >= 0)
				xe_movaps_xm(xmmX.Id, &::vuRegs[index].VF[vfLoadReg]);

			xmmMap[x].VFreg = vfLoadReg;
			xmmMap[x].xyzw  = 0;
		}
		xmmMap[x].isZero = (vfLoadReg == 0);
		xmmMap[x].count    = counter;
		xmmMap[x].isNeeded = true;
		updateCOP2AllocState(x);
		return xmmX;
	}

	void clearGPR(const xRegisterInt& reg) { clearGPR(reg.Id); }

	void clearGPR(int regId)
	{
		microMapGPR& clear = gprMap[regId];

		if (regAllocCOP2)
		{
			if (x86regs[regId].inuse && x86regs[regId].type == X86TYPE_VIREG)
				_freeX86regWithoutWriteback(regId);
		}

		clear.VIreg = -1;
		clear.count = 0;
		clear.isNeeded = 0;
		clear.dirty = false;
		clear.isZeroExtended = false;
	}

	void clearGPRCOP2(int regId)
	{
		if (regAllocCOP2)
			clearGPR(regId);
	}

	void updateCOP2AllocState(const xRegisterInt& reg)
	{
		if (!regAllocCOP2)
			return;

		const u32 rn     = reg.Id;
		const bool dirty = (gprMap[rn].VIreg >= 0 && gprMap[rn].dirty);
		x86regs[rn].reg = gprMap[rn].VIreg;
		x86regs[rn].counter = gprMap[rn].count;
		x86regs[rn].mode = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		x86regs[rn].needed = gprMap[rn].isNeeded;
	}

	void writeBackReg(const xRegisterInt& reg, bool clearDirty)
	{
		microMapGPR& mapX = gprMap[reg.Id];
		if (mapX.dirty)
		{
			if (mapX.VIreg < 16)
				xe_mov16_mr(&::vuRegs[index].VI[mapX.VIreg], reg.Id);
			if (clearDirty)
			{
				mapX.dirty = false;
				updateCOP2AllocState(reg);
			}
		}
	}

	void clearNeeded(const xRegisterInt& reg)
	{
		microMapGPR& clear = gprMap[reg.Id];
		clear.isNeeded = false;
		if (regAllocCOP2)
			x86regs[reg.Id].needed = false;
	}

	void unbindAnyVIAllocations(int reg, bool& backup)
	{
		for (int i = 0; i < gprTotal; i++)
		{
			microMapGPR& mapI = gprMap[i];
			if (mapI.VIreg == reg)
			{
				if (backup)
				{
					writeVIBackup(xRegister32(i));
					backup = false;
				}

				// if it's needed, we just unbind the allocation and preserve it, otherwise clear
				if (mapI.isNeeded)
				{
					if (regAllocCOP2)
					{
						x86regs[i].reg = -1;
					}

					mapI.VIreg = -1;
					mapI.dirty = false;
					mapI.isZeroExtended = false;
				}
				else
				{
					clearGPR(i);
				}

				break;
			}
		}
	}

	const xRegister32& allocGPR(int viLoadReg = -1, int viWriteReg = -1, bool backup = false, bool zext_if_dirty = false)
	{
		// TODO: When load != write, we should check whether load is used later, and if so, copy it.

		const int this_counter = regAllocCOP2 ? (g_x86AllocCounter++) : (counter++);
		if (viLoadReg == 0 || viWriteReg == 0)
		{
			// write zero register as temp and discard later
			if (viWriteReg == 0)
			{
				int x = findFreeGPR(-1);
				const xRegister32& gprX = xRegister32::GetInstance(x);
				writeBackReg(gprX, true);
				xe_xor32_rr(gprX.Id, gprX.Id);
				gprMap[x].VIreg = -1;
				gprMap[x].dirty = false;
				gprMap[x].count = this_counter;
				gprMap[x].isNeeded = true;
				gprMap[x].isZeroExtended = true;
				return gprX;
			}
		}

		if (viLoadReg >= 0) // Search For Cached Regs
		{
			for (int i = 0; i < gprTotal; i++)
			{
				microMapGPR& mapI = gprMap[i];
				if (mapI.VIreg == viLoadReg)
				{
					// Do this first, there is a case where when loadReg != writeReg, the findFreeGPR can steal the loadReg
					gprMap[i].count = this_counter;

					if (viWriteReg >= 0) // Reg will be modified
					{
						if (viLoadReg != viWriteReg)
						{
							// kill any allocations of viWriteReg
							unbindAnyVIAllocations(viWriteReg, backup);

							// allocate a new register for writing to
							int x = findFreeGPR(viWriteReg);
							const xRegister32& gprX = xRegister32::GetInstance(x);

							writeBackReg(gprX, true);

							// writeReg not cached, needs backing up
							if (backup && gprMap[x].VIreg != viWriteReg)
							{
								xe_movzx32_rm16(gprX.Id, &::vuRegs[index].VI[viWriteReg]);
								writeVIBackup(gprX);
								backup = false;
							}

							if (zext_if_dirty)
								xe_movzx32_rr16(gprX.Id, i);
							else
								xe_mov32_rr(gprX.Id, i);
							gprMap[x].isZeroExtended = zext_if_dirty;
							std::swap(x, i);
						}
						else
						{
							// writing to it, no longer zero extended
							gprMap[i].isZeroExtended = false;
						}

						gprMap[i].VIreg = viWriteReg;
						gprMap[i].dirty = true;
					}
					else if (zext_if_dirty && !gprMap[i].isZeroExtended)
					{
						xe_movzx32_rr16(i, i);
						gprMap[i].isZeroExtended = true;
					}

					gprMap[i].isNeeded = true;

					if (backup)
						writeVIBackup(xRegister32(i));

					if (regAllocCOP2)
					{
						x86regs[i].reg = gprMap[i].VIreg;
						x86regs[i].mode = gprMap[i].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
					}

					return xRegister32::GetInstance(i);
				}
			}
		}

		if (viWriteReg >= 0) // Writing a new value, make sure this register isn't cached already
			unbindAnyVIAllocations(viWriteReg, backup);

		int x = findFreeGPR(viLoadReg);
		const xRegister32& gprX = xRegister32::GetInstance(x);
		writeBackReg(gprX, true);

		// Special case: we need to back up the destination register, but it might not have already
		// been cached. If so, we need to load the old value from state and back it up. Otherwise,
		// it's going to get lost when we eventually write this register back.
		if (backup && viLoadReg >= 0 && viWriteReg > 0 && viLoadReg != viWriteReg)
		{
			xe_movzx32_rm16(gprX.Id, &::vuRegs[index].VI[viWriteReg]);
			writeVIBackup(gprX);
			backup = false;
		}

		if (viLoadReg > 0)
			xe_movzx32_rm16(gprX.Id, &::vuRegs[index].VI[viLoadReg]);
		else if (viLoadReg == 0)
			xe_xor32_rr(gprX.Id, gprX.Id);

		gprMap[x].VIreg = viLoadReg;
		gprMap[x].isZeroExtended = true;
		if (viWriteReg >= 0)
		{
			gprMap[x].VIreg = viWriteReg;
			gprMap[x].dirty = true;
			gprMap[x].isZeroExtended = false;

			if (backup)
			{
				if (viLoadReg < 0 && viWriteReg > 0)
					xe_movzx32_rm16(gprX.Id, &::vuRegs[index].VI[viWriteReg]);
				writeVIBackup(gprX);
			}
		}

		gprMap[x].count = this_counter;
		gprMap[x].isNeeded = true;

		if (regAllocCOP2)
		{
			x86regs[x].reg = gprMap[x].VIreg;
			x86regs[x].mode = gprMap[x].dirty ? (MODE_WRITE | MODE_READ) : (MODE_READ);
		}

		return gprX;
	}

	void moveVIToGPR(const xRegisterInt& reg, int vi, bool signext = false)
	{
		if (vi == 0)
		{
			xe_xor32_rr(reg.Id, reg.Id);
			return;
		}

		// TODO: Check liveness/usedness before allocating.
		// TODO: Check whether zero-extend is needed everywhere heae. Loadstores are.
		const xRegister32& srcreg = allocGPR(vi);
		if (signext)
			xe_movsx32_rr16(reg.Id, srcreg.Id);
		else
			xe_movzx32_rr16(reg.Id, srcreg.Id);
		clearNeeded(srcreg);
	}
};
