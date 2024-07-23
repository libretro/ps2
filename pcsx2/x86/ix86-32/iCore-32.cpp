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

#include <cstring> /* memset */

#include "../../R3000A.h"
#include "../../Memory.h"
#include "../../VU.h"
#include "../../Vif.h"
#include "../iR3000A.h"
#include "../iR5900.h"

#include "common/emitter/x86emitter.h"

using namespace x86Emitter;

// yay sloppy crap needed until we can remove dependency on this hippopotamic
// landmass of shared code. (air)
extern u32 g_psxConstRegs[32];

// X86 caching
static uint g_x86checknext;

// use special x86 register allocation for ia32

void _initX86regs(void)
{
	memset(x86regs, 0, sizeof(x86regs));
	g_x86AllocCounter = 0;
	g_x86checknext = 0;
}

static int _getFreeX86reg(int mode)
{
	int tempi = -1;
	u32 bestcount = 0x10000;

	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		const int reg = (g_x86checknext + i) % iREGCNT_GPR;
		if (x86regs[reg].inuse || !_isAllocatableX86reg(reg))
			continue;

		if ((mode & MODE_CALLEESAVED) && xRegister32::IsCallerSaved(reg))
			continue;

		if ((mode & MODE_COP2) && mVUIsReservedCOP2(reg))
			continue;

		if (x86regs[reg].inuse == 0)
		{
			g_x86checknext = (reg + 1) % iREGCNT_GPR;
			return reg;
		}
	}

	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (!_isAllocatableX86reg(i))
			continue;

		if ((mode & MODE_CALLEESAVED) && xRegister32::IsCallerSaved(i))
			continue;

		if ((mode & MODE_COP2) && mVUIsReservedCOP2(i))
			continue;

		if (x86regs[i].needed)
			continue;

		if (x86regs[i].type != X86TYPE_TEMP)
		{

			if (x86regs[i].counter < bestcount)
			{
				tempi = static_cast<int>(i);
				bestcount = x86regs[i].counter;
			}
			continue;
		}

		_freeX86reg(i);
		return i;
	}

	if (tempi != -1)
	{
		_freeX86reg(tempi);
		return tempi;
	}

	return -1;
}

void _flushConstReg(int reg)
{
	if (GPR_IS_CONST1(reg) && !(g_cpuFlushedConstReg & (1 << reg)))
	{
		xWriteImm64ToMem(&cpuRegs.GPR.r[reg].UD[0], rax, g_cpuConstRegs[reg].SD[0]);
		g_cpuFlushedConstReg |= (1 << reg);
	}
}

void _flushConstRegs(void)
{
	int zero_reg_count = 0;
	int minusone_reg_count = 0;
	for (u32 i = 0; i < 32; i++)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

		if (g_cpuConstRegs[i].SD[0] == 0)
			zero_reg_count++;
		else if (g_cpuConstRegs[i].SD[0] == -1)
			minusone_reg_count++;
	}

	// if we have more than one of zero/minus-one, precompute
	bool rax_is_zero = false;
	if (zero_reg_count > 1)
	{
		xXOR(eax, eax);
		for (u32 i = 0; i < 32; i++)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == 0)
			{
				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
				g_cpuFlushedConstReg |= 1u << i;
			}
		}
		rax_is_zero = true;
	}
	if (minusone_reg_count > 1)
	{
		if (!rax_is_zero)
			xMOV(rax, -1);
		else
			xNOT(rax);

		for (u32 i = 0; i < 32; i++)
		{
			if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
				continue;

			if (g_cpuConstRegs[i].SD[0] == -1)
			{
				xMOV(ptr64[&cpuRegs.GPR.r[i].UD[0]], rax);
				g_cpuFlushedConstReg |= 1u << i;
			}
		}
	}

	// and whatever's left over..
	for (u32 i = 0; i < 32; i++)
	{
		if (!GPR_IS_CONST1(i) || g_cpuFlushedConstReg & (1u << i))
			continue;

		xWriteImm64ToMem(&cpuRegs.GPR.r[i].UD[0], rax, g_cpuConstRegs[i].UD[0]);
		g_cpuFlushedConstReg |= 1u << i;
	}
}

int _allocX86reg(int type, int reg, int mode)
{
	int hostXMMreg = (type == X86TYPE_GPR) ? _checkXMMreg(XMMTYPE_GPRREG, reg, 0) : -1;
	if (type != X86TYPE_TEMP)
	{
		for (int i = 0; i < static_cast<int>(iREGCNT_GPR); i++)
		{
			if (!x86regs[i].inuse || x86regs[i].type != type || x86regs[i].reg != reg)
				continue;

			if (type == X86TYPE_GPR)
			{
				if (mode & MODE_WRITE)
				{
					if (GPR_IS_CONST1(reg))
						g_cpuHasConstReg &= ~(1 << (reg));

					if (hostXMMreg >= 0)
					{
						// ensure upper bits get written
						_freeXMMreg(hostXMMreg);
					}
				}
			}
			else if (type == X86TYPE_PSX)
			{
				if (mode & MODE_WRITE)
				{
					if (PSX_IS_CONST1(reg))
					{
						PSX_DEL_CONST(reg);
					}
				}
			}
			else if (type == X86TYPE_VIREG)
			{
				// keep VI temporaries separate
				if (reg < 0)
					continue;
			}

			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].mode |= mode & ~MODE_CALLEESAVED;
			x86regs[i].needed = true;
			return i;
		}
	}

	const int regnum = _getFreeX86reg(mode);
	xRegister64 new_reg(regnum);
	x86regs[regnum].type = type;
	x86regs[regnum].reg = reg;
	x86regs[regnum].mode = mode & ~MODE_CALLEESAVED;
	x86regs[regnum].counter = g_x86AllocCounter++;
	x86regs[regnum].needed = true;
	x86regs[regnum].inuse = true;

	if (mode & MODE_READ)
	{
		switch (type)
		{
			case X86TYPE_GPR:
			{
				if (reg == 0)
				{
					xXOR(xRegister32(new_reg), xRegister32(new_reg)); // 32-bit is smaller and zexts anyway
				}
				else
				{
					if (hostXMMreg >= 0)
					{
						// is in a XMM. we don't need to free the XMM since we're not writing, and it's still valid
						xMOVD(new_reg, xRegisterSSE(hostXMMreg)); // actually MOVQ

						// if the XMM was dirty, just get rid of it, we don't want to try to sync the values up...
						if (xmmregs[hostXMMreg].mode & MODE_WRITE)
						{
							_freeXMMreg(hostXMMreg);
						}
					}
					else if (GPR_IS_CONST1(reg))
					{
						xMOV64(new_reg, g_cpuConstRegs[reg].SD[0]);
						g_cpuFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty
					}
					else
					{
						// not loaded
						xMOV(new_reg, ptr64[&cpuRegs.GPR.r[reg].UD[0]]);
					}
				}
			}
			break;

			case X86TYPE_FPRC:
				xMOV(xRegister32(regnum), ptr32[&fpuRegs.fprc[reg]]);
				break;

			case X86TYPE_PSX:
			{
				const xRegister32 new_reg32(regnum);
				if (reg == 0)
				{
					xXOR(new_reg32, new_reg32);
				}
				else
				{
					if (PSX_IS_CONST1(reg))
					{
						xMOV(new_reg32, g_psxConstRegs[reg]);
						g_psxFlushedConstReg |= (1u << reg);
						x86regs[regnum].mode |= MODE_WRITE; // reg is dirty
					}
					else
					{
						xMOV(new_reg32, ptr32[&psxRegs.GPR.r[reg]]);
					}
				}
			}
			break;

			case X86TYPE_VIREG:
			{
				xMOVZX(xRegister32(regnum), ptr16[&vuRegs[0].VI[reg].US[0]]);
			}
			break;

			default:
				abort();
				break;
		}
	}

	if (type == X86TYPE_GPR && (mode & MODE_WRITE))
	{
		if (GPR_IS_CONST1(reg))
			g_cpuHasConstReg &= ~(1 << (reg));
		if (hostXMMreg >= 0)
		{
			// writing, so kill the xmm allocation. gotta ensure the upper bits gets stored first.
			_freeXMMreg(hostXMMreg);
		}
	}
	else if (type == X86TYPE_PSX && (mode & MODE_WRITE))
	{
		if (PSX_IS_CONST1(reg))
			g_psxHasConstReg &= ~(1 << (reg));
	}

	return regnum;
}

void _writebackX86Reg(int x86reg)
{
	switch (x86regs[x86reg].type)
	{
		case X86TYPE_GPR:
			xMOV(ptr64[&cpuRegs.GPR.r[x86regs[x86reg].reg].UD[0]], xRegister64(x86reg));
			break;

		case X86TYPE_FPRC:
			xMOV(ptr32[&fpuRegs.fprc[x86regs[x86reg].reg]], xRegister32(x86reg));
			break;

		case X86TYPE_VIREG:
			xMOV(ptr16[&vuRegs[0].VI[x86regs[x86reg].reg].UL], xRegister16(x86reg));
			break;

		case X86TYPE_PCWRITEBACK:
			xMOV(ptr32[&cpuRegs.pcWriteback], xRegister32(x86reg));
			break;

		case X86TYPE_PSX:
			xMOV(ptr32[&psxRegs.GPR.r[x86regs[x86reg].reg]], xRegister32(x86reg));
			break;

		case X86TYPE_PSX_PCWRITEBACK:
			xMOV(ptr32[&psxRegs.pcWriteback], xRegister32(x86reg));
			break;

		default:
			abort();
			break;
	}
}

int _checkX86reg(int type, int reg, int mode)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].inuse && x86regs[i].reg == reg && x86regs[i].type == type)
		{
			// ensure constants get deleted once we alloc as write
			if (mode & MODE_WRITE)
			{
				// go through the alloc path instead, because we might need to invalidate an xmm.
				if (type == X86TYPE_GPR)
					return _allocX86reg(X86TYPE_GPR, reg, mode);
				else if (type == X86TYPE_PSX)
				{
					PSX_DEL_CONST(reg);
				}
			}

			x86regs[i].mode |= mode;
			x86regs[i].counter = g_x86AllocCounter++;
			x86regs[i].needed = 1;
			return i;
		}
	}

	return -1;
}

void _addNeededX86reg(int type, int reg)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse || x86regs[i].reg != reg || x86regs[i].type != type)
			continue;

		x86regs[i].counter = g_x86AllocCounter++;
		x86regs[i].needed = 1;
	}
}

void _clearNeededX86regs(void)
{
	for (uint i = 0; i < iREGCNT_GPR; i++)
	{
		if (x86regs[i].needed)
		{
			if (x86regs[i].inuse && (x86regs[i].mode & MODE_WRITE))
				x86regs[i].mode |= MODE_READ;
		}
		x86regs[i].needed = 0;
	}
}

void _freeX86reg(const x86Emitter::xRegister32& x86reg)
{
	_freeX86reg(x86reg.Id);
}

void _freeX86reg(int x86reg)
{
	if (x86regs[x86reg].inuse && (x86regs[x86reg].mode & MODE_WRITE))
	{
		_writebackX86Reg(x86reg);
		x86regs[x86reg].mode &= ~MODE_WRITE;
	}

	_freeX86regWithoutWriteback(x86reg);
}

void _freeX86regWithoutWriteback(int x86reg)
{
	x86regs[x86reg].inuse = 0;

	if (x86regs[x86reg].type == X86TYPE_VIREG)
		mVUFreeCOP2GPR(x86reg);
}

void _flushX86regs(void)
{
	for (u32 i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse && x86regs[i].mode & MODE_WRITE)
		{
			_writebackX86Reg(i);
			x86regs[i].mode = (x86regs[i].mode & ~MODE_WRITE) | MODE_READ;
		}
	}
}
