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

#include "common/emitter/x86emitter.h"
#include "VUmicro.h"

// Namespace Note : iCore32 contains all of the Register Allocation logic, in addition to a handful
// of utility functions for emitting frequent code.

////////////////////////////////////////////////////////////////////////////////
// Shared Register allocation flags (apply to X86, XMM, MMX, etc).

#define MODE_READ        1
#define MODE_WRITE       2
#define MODE_CALLEESAVED  0x20 // can't flush reg to mem
#define MODE_COP2 0x40 // don't allow using reserved VU registers

#define PROCESS_EE_XMM 0x02

#define PROCESS_EE_S 0x04 // S is valid, otherwise take from mem
#define PROCESS_EE_T 0x08 // T is valid, otherwise take from mem
#define PROCESS_EE_D 0x10 // D is valid, otherwise take from mem

#define PROCESS_EE_LO         0x40 // lo reg is valid
#define PROCESS_EE_HI         0x80 // hi reg is valid
#define PROCESS_EE_ACC        0x40 // acc reg is valid

#define EEREC_S    (((info) >>  8) & 0xf)
#define EEREC_T    (((info) >> 12) & 0xf)
#define EEREC_D    (((info) >> 16) & 0xf)
#define EEREC_LO   (((info) >> 20) & 0xf)
#define EEREC_HI   (((info) >> 24) & 0xf)
#define EEREC_ACC  (((info) >> 20) & 0xf)

#define PROCESS_EE_SET_S(reg)   (((reg) <<  8) | PROCESS_EE_S)
#define PROCESS_EE_SET_T(reg)   (((reg) << 12) | PROCESS_EE_T)
#define PROCESS_EE_SET_D(reg)   (((reg) << 16) | PROCESS_EE_D)
#define PROCESS_EE_SET_LO(reg)  (((reg) << 20) | PROCESS_EE_LO)
#define PROCESS_EE_SET_HI(reg)  (((reg) << 24) | PROCESS_EE_HI)
#define PROCESS_EE_SET_ACC(reg) (((reg) << 20) | PROCESS_EE_ACC)

// special info not related to above flags
#define PROCESS_CONSTS 1
#define PROCESS_CONSTT 2

// XMM caching helpers
enum xmminfo : u16 
{
	XMMINFO_READLO = 0x001,
	XMMINFO_READHI = 0x002,
	XMMINFO_WRITELO = 0x004,
	XMMINFO_WRITEHI = 0x008,
	XMMINFO_WRITED = 0x010,
	XMMINFO_READD = 0x020,
	XMMINFO_READS = 0x040,
	XMMINFO_READT = 0x080,
	XMMINFO_READACC = 0x200,
	XMMINFO_WRITEACC = 0x400,
	XMMINFO_WRITET = 0x800,

	XMMINFO_64BITOP = 0x1000,
	XMMINFO_FORCEREGS = 0x2000,
	XMMINFO_FORCEREGT = 0x4000,
	XMMINFO_NORENAME = 0x8000 // disables renaming of Rs to Rt in Rt = Rs op imm
};

////////////////////////////////////////////////////////////////////////////////
//   X86 (32-bit) Register Allocation Tools

enum x86type : u8 
{
	X86TYPE_TEMP = 0,
	X86TYPE_GPR = 1,
	X86TYPE_FPRC = 2,
	X86TYPE_VIREG = 3,
	X86TYPE_PCWRITEBACK = 4,
	X86TYPE_PSX = 5,
	X86TYPE_PSX_PCWRITEBACK = 6
};

struct _x86regs
{
	u8 inuse;
	s8 reg;
	u8 mode;
	u8 needed;
	u8 type; // X86TYPE_
	u16 counter;
	u32 extra; // extra info assoc with the reg
};

extern _x86regs x86regs[iREGCNT_GPR], s_saveX86regs[iREGCNT_GPR];

bool _isAllocatableX86reg(int x86reg);
void _initX86regs(void);
int _allocX86reg(int type, int reg, int mode);
int _checkX86reg(int type, int reg, int mode);
bool _hasX86reg(int type, int reg, int required_mode = 0);
void _addNeededX86reg(int type, int reg);
void _clearNeededX86regs();
void _freeX86reg(const x86Emitter::xRegister32& x86reg);
void _freeX86reg(int x86reg);
void _freeX86regWithoutWriteback(int x86reg);
void _freeX86regs();
void _flushX86regs();
void _flushConstRegs();
void _flushConstReg(int reg);
void _writebackX86Reg(int x86reg);

void mVUFreeCOP2GPR(int hostreg);
bool mVUIsReservedCOP2(int hostreg);

////////////////////////////////////////////////////////////////////////////////
//   XMM (128-bit) Register Allocation Tools

#define XMMTYPE_TEMP   0 // has to be 0
#define XMMTYPE_GPRREG X86TYPE_GPR
#define XMMTYPE_FPREG  6
#define XMMTYPE_FPACC  7
#define XMMTYPE_VFREG  8

// lo and hi regs
#define XMMGPR_LO  33
#define XMMGPR_HI  32
#define XMMFPU_ACC 32

enum : int
{
	DELETE_REG_FREE = 0,
	DELETE_REG_FLUSH = 1,
	DELETE_REG_FLUSH_AND_FREE = 2,
	DELETE_REG_FREE_NO_WRITEBACK = 3
};

struct _xmmregs
{
	uint8_t inuse;
	int8_t reg;
	uint8_t type;
	uint8_t mode;
	uint8_t needed;
	uint16_t counter;
};

void _initXMMregs(void);
int _allocTempXMMreg(XMMSSEType type);
int _allocFPtoXMMreg(int fpreg, int mode);
int _allocGPRtoXMMreg(int gprreg, int mode);
int _allocFPACCtoXMMreg(int mode);
void _reallocateXMMreg(int xmmreg, int newtype, int newreg, int newmode, bool writeback = true);
int _checkXMMreg(int type, int reg, int mode);
bool _hasXMMreg(int type, int reg, int required_mode = 0);
void _addNeededFPtoXMMreg(int fpreg);
void _addNeededFPACCtoXMMreg();
void _addNeededGPRtoX86reg(int gprreg);
void _addNeededPSXtoX86reg(int gprreg);
void _addNeededGPRtoXMMreg(int gprreg);
void _clearNeededXMMregs();
void _deleteGPRtoX86reg(int reg, int flush);
void _deletePSXtoX86reg(int reg, int flush);
void _deleteGPRtoXMMreg(int reg, int flush);
void _deleteFPtoXMMreg(int reg, int flush);
void _freeXMMreg(int xmmreg);
void _freeXMMregWithoutWriteback(int xmmreg);
void _writebackXMMreg(int xmmreg);
int _allocVFtoXMMreg(int vfreg, int mode);
void mVUFreeCOP2XMMreg(int hostreg);
void _flushCOP2regs(void);
void _flushXMMreg(int xmmreg);
void _flushXMMregs(void);

//////////////////////
// Instruction Info //
//////////////////////
// Liveness information for the noobs :)
// Let's take I instructions that read from RN register set and write to
// WN register set.
// 1/ EEINST_USED will be set in register N of instruction I1, if and only if RN or WN is used in the insruction I2 with I2 >= I1.
// In others words, it will be set on [I0, ILast] with ILast the last instruction that use the register.
// 2/ EEINST_LASTUSE will be set in register N the last instruction that use the register.
// Note: EEINST_USED will be cleared after EEINST_LASTUSE
// My guess: both variable allow to detect register that can be flushed for free
//
// 3/ EEINST_LIVE* is cleared when register is written. And set again when register is read.
// My guess: the purpose is to detect the usage hole in the flow

#define EEINST_LIVE      1 /* if var is ever used (read or write) */
#define EEINST_LASTUSE   8 /* if var isn't written/read anymore */
#define EEINST_XMM    0x20 /* var will be used in xmm ops */
#define EEINST_USED   0x40

#define EEINST_COP2_DENORMALIZE_STATUS_FLAG 0x100
#define EEINST_COP2_NORMALIZE_STATUS_FLAG 0x200
#define EEINST_COP2_STATUS_FLAG 0x400
#define EEINST_COP2_MAC_FLAG 0x800
#define EEINST_COP2_CLIP_FLAG 0x1000
#define EEINST_COP2_SYNC_VU0 0x2000
#define EEINST_COP2_FINISH_VU0 0x4000
#define EEINST_COP2_FLUSH_VU0_REGISTERS 0x8000

/*************************************************************************
 * iFlushCall / _psxFlushCall Parameters
 ************************************************************************/

#define FLUSH_NONE             0x000 /* frees caller saved registers */
#define FLUSH_CONSTANT_REGS    0x001
#define FLUSH_FLUSH_XMM        0x002
#define FLUSH_FREE_XMM         0x004 /* both flushes and frees */
#define FLUSH_ALL_X86          0x020 /* flush x86 */
#define FLUSH_FREE_TEMP_X86    0x040 /* flush and free temporary x86 regs */
#define FLUSH_FREE_NONTEMP_X86 0x080 /* free all x86 regs, except temporary */
#define FLUSH_FREE_VU0         0x100 /* free all vu0 related regs */
#define FLUSH_PC               0x200 /* program counter */
#if 0
#define FLUSH_CAUSE            0x000 /* disabled for now: cause register, only the branch delay bit */
#endif
#define FLUSH_CODE             0x800 /* opcode for interpreter */

#define FLUSH_EVERYTHING   0x1ff
#if 0
#define FLUSH_EXCEPTION	   0x1ff     /* will probably do this totally differently actually */
#endif
#define FLUSH_INTERPRETER  0xfff
#define FLUSH_FULLVTLB 0x000

/* no freeing, used when callee won't destroy xmm regs */
#define FLUSH_NODESTROY (FLUSH_CONSTANT_REGS | FLUSH_FLUSH_XMM | FLUSH_ALL_X86)

/* If unset, values which are not live will not be written back to memory.
 * Tends to break stuff at the moment. */
#define EE_WRITE_DEAD_VALUES 1

/* Returns true if the register is used later in the block, 
 * and this isn't the last instruction to use it.
 * In other words, the register is worth keeping in a host register/caching it. */
#define EEINST_USEDTEST(reg) ((g_pCurInstInfo->regs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED)

/* Returns true if the register is used later in the block as an XMM/128-bit value. */
#define EEINST_XMMUSEDTEST(reg) ((g_pCurInstInfo->regs[reg] & (EEINST_USED | EEINST_XMM | EEINST_LASTUSE)) == (EEINST_USED | EEINST_XMM))

/* Returns true if the specified VF register is used later in the block. */
#define EEINST_VFUSEDTEST(reg) ((g_pCurInstInfo->vfregs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED)

/* Returns true if the value should be computed/written back.
 * Basically, this means it's either used before it's overwritten, or not overwritten by the end of the block. */
#define EEINST_LIVETEST(reg) (EE_WRITE_DEAD_VALUES || ((g_pCurInstInfo->regs[reg] & EEINST_LIVE) != 0))

/* Returns true if the register can be renamed into another. */
#define EEINST_RENAMETEST(reg) ((reg) == 0 || !EEINST_USEDTEST(reg) || !EEINST_LIVETEST(reg))

#define FPUINST_ISLIVE(reg)  (!!(g_pCurInstInfo->fpuregs[reg] & EEINST_LIVE))
#define FPUINST_LASTUSE(reg) (!!(g_pCurInstInfo->fpuregs[reg] & EEINST_LASTUSE))

/* Returns true if the register is used later in the block, and this isn't the 
 * last instruction to use it.
 * In other words, the register is worth keeping in a host register/caching it. */
#define FPUINST_USEDTEST(reg) ((g_pCurInstInfo->fpuregs[reg] & (EEINST_USED | EEINST_LASTUSE)) == EEINST_USED)

/* Returns true if the value should be computed/written back. */
#define FPUINST_LIVETEST(reg) (EE_WRITE_DEAD_VALUES || FPUINST_ISLIVE(reg))

/* Returns true if the register can be renamed into another. */
#define FPUINST_RENAMETEST(reg) ((!EEINST_USEDTEST(reg) || !EEINST_LIVETEST(reg)))

struct EEINST
{
	uint16_t info;       /* extra info, if 1 inst is COP1, 2 inst is COP2. Also uses EEINST_XMM */
	uint8_t regs[34];    /* includes HI/LO (HI=32, LO=33) */
	uint8_t fpuregs[33]; /* ACC=32 */
	uint8_t vfregs[34];  /* ACC=32, I=33 */
	uint8_t viregs[16];

	/* uses XMMTYPE_ flags; if type == XMMTYPE_TEMP, not used */
	uint8_t writeType[3], writeReg[3]; /* reg written in this inst, 0 if no reg */
	uint8_t readType[4], readReg[4];
};

extern EEINST* g_pCurInstInfo; // info for the cur instruction
extern void _recClearInst(EEINST* pinst);


extern _xmmregs xmmregs[iREGCNT_XMM], s_saveXMMregs[iREGCNT_XMM];

extern u16 g_x86AllocCounter;

/* Allocates only if later insts use this register */
int _allocIfUsedGPRtoX86(int gprreg, int mode);
int _allocIfUsedVItoX86(int vireg, int mode);
int _allocIfUsedGPRtoXMM(int gprreg, int mode);
int _allocIfUsedFPUtoXMM(int fpureg, int mode);
