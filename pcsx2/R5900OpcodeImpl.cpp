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


#include "Common.h"

#include <cstring> /* memset */
#include <float.h>

#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "GS.h"
#include "CDVD/CDVD.h"
#include "BiosTools.h"
#include "VMManager.h"

GS_VideoMode gsVideoMode = GS_VideoMode::Uninitialized;

static __fi bool _add64_Overflow(int64_t x, int64_t y, int64_t *ret)
{
	const int64_t result = x + y;

	// Let's all give gigaherz a big round of applause for finding this gem,
	// which apparently works, and generates compact/fast x86 code too (the
	// other method below is like 5-10 times slower).

	if( ((~(x ^ y)) & (x ^ result)) < 0 )
	{
		cpuException(0x30, cpuRegs.branch);		// fixme: is 0x30 right for overflow??
		return true;
	}

	*ret = result;
	return false;
}

static __fi bool _add32_Overflow( int32_t x, int32_t y, int64_t *ret)
{
	GPR_reg64 result;
	result.SD[0] = (int64_t)x + y;

	// This 32bit method can rely on the MIPS documented method of checking for
	// overflow, whichs imply compares bit 32 (rightmost bit of the upper word),
	// against bit 31 (leftmost of the lower word).

	// If bit32 != bit31 then we have an overflow.
	if((result.UL[0]>>31) != (result.UL[1] & 1))
	{
		cpuException(0x30, cpuRegs.branch);
		return true;
	}

	*ret = result.SD[0];

	return false;
}


const R5900::OPCODE& R5900::GetCurrentInstruction()
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[_Opcode_];
	while (opcode->getsubclass)
		opcode = &opcode->getsubclass(cpuRegs.code);
	return *opcode;
}

const R5900::OPCODE& R5900::GetInstruction(u32 op)
{
	const OPCODE* opcode = &R5900::OpcodeTables::tbl_Standard[op >> 26];
	while (opcode->getsubclass)
		opcode = &opcode->getsubclass(op);
	return *opcode;
}

const char * const R5900::bios[256]=
{
/* 0x00 */
	"RFU000_FullReset", "ResetEE",				"SetGsCrt",				"RFU003",
	"Exit",				"RFU005",				"LoadExecPS2",			"ExecPS2",
	"RFU008",			"RFU009",				"AddSbusIntcHandler",	"RemoveSbusIntcHandler",
	"Interrupt2Iop",	"SetVTLBRefillHandler", "SetVCommonHandler",	"SetVInterruptHandler",
/* 0x10 */
	"AddIntcHandler",	"RemoveIntcHandler",	"AddDmacHandler",		"RemoveDmacHandler",
	"_EnableIntc",		"_DisableIntc",			"_EnableDmac",			"_DisableDmac",
	"_SetAlarm",		"_ReleaseAlarm",		"_iEnableIntc",			"_iDisableIntc",
	"_iEnableDmac",		"_iDisableDmac",		"_iSetAlarm",			"_iReleaseAlarm",
/* 0x20 */
	"CreateThread",			"DeleteThread",		"StartThread",			"ExitThread",
	"ExitDeleteThread",		"TerminateThread",	"iTerminateThread",		"DisableDispatchThread",
	"EnableDispatchThread",		"ChangeThreadPriority", "iChangeThreadPriority",	"RotateThreadReadyQueue",
	"iRotateThreadReadyQueue",	"ReleaseWaitThread",	"iReleaseWaitThread",		"GetThreadId",
/* 0x30 */
	"ReferThreadStatus","iReferThreadStatus",	"SleepThread",		"WakeupThread",
	"_iWakeupThread",   "CancelWakeupThread",	"iCancelWakeupThread",	"SuspendThread",
	"iSuspendThread",   "ResumeThread",		"iResumeThread",	"JoinThread",
	"RFU060",	    "RFU061",			"EndOfHeap",		 "RFU063",
/* 0x40 */
	"CreateSema",	    "DeleteSema",	"SignalSema",		"iSignalSema",
	"WaitSema",	    "PollSema",		"iPollSema",		"ReferSemaStatus",
	"iReferSemaStatus", "RFU073",		"SetOsdConfigParam", 	"GetOsdConfigParam",
	"GetGsHParam",	    "GetGsVParam",	"SetGsHParam",		"SetGsVParam",
/* 0x50 */
	"RFU080_CreateEventFlag",	"RFU081_DeleteEventFlag",
	"RFU082_SetEventFlag",		"RFU083_iSetEventFlag",
	"RFU084_ClearEventFlag",	"RFU085_iClearEventFlag",
	"RFU086_WaitEventFlag",		"RFU087_PollEventFlag",
	"RFU088_iPollEventFlag",	"RFU089_ReferEventFlagStatus",
	"RFU090_iReferEventFlagStatus", "RFU091_GetEntryAddress",
	"EnableIntcHandler_iEnableIntcHandler",
	"DisableIntcHandler_iDisableIntcHandler",
	"EnableDmacHandler_iEnableDmacHandler",
	"DisableDmacHandler_iDisableDmacHandler",
/* 0x60 */
	"KSeg0",				"EnableCache",	"DisableCache",			"GetCop0",
	"FlushCache",			"RFU101",		"CpuConfig",			"iGetCop0",
	"iFlushCache",			"RFU105",		"iCpuConfig", 			"sceSifStopDma",
	"SetCPUTimerHandler",	"SetCPUTimer",	"SetOsdConfigParam2",	"GetOsdConfigParam2",
/* 0x70 */
	"GsGetIMR_iGsGetIMR",				"GsGetIMR_iGsPutIMR",	"SetPgifHandler", 				"SetVSyncFlag",
	"RFU116",							"print", 				"sceSifDmaStat_isceSifDmaStat", "sceSifSetDma_isceSifSetDma",
	"sceSifSetDChain_isceSifSetDChain", "sceSifSetReg",			"sceSifGetReg",					"ExecOSD",
	"Deci2Call",						"PSMode",				"MachineType",					"GetMemorySize",
};

static u32 deci2addr = 0;
static u32 deci2handler = 0;
static char deci2buffer[256];

void Deci2Reset(void)
{
	deci2handler	= 0;
	deci2addr	= 0;
	memset(deci2buffer, 0, sizeof(deci2buffer));
}

bool SaveStateBase::deci2Freeze()
{
	if (!(FreezeTag( "deci2" )))
		return false;

	Freeze( deci2addr );
	Freeze( deci2handler );
	Freeze( deci2buffer );

	return IsOkay();
}

/*
 *	int Deci2Call(int, u_int *);
 *
 *  HLE implementation of the Deci2 interface.
 */

static int __Deci2Call(int call, u32 *addr)
{
	if (call > 0x10)
		return -1;

	switch (call)
	{
		case 1: /* open */
			if(addr)
			{
				deci2addr    = addr[1];
				deci2handler = addr[2];
			}
			else
				deci2handler = 0;
			return 1;

		case 2: // close
			deci2addr = 0;
			deci2handler = 0;
			return 1;

		case 3: // reqsend
		{
			const uint32_t *d2ptr;
			if (!deci2addr)
				return 1;

			d2ptr = (u32*)PSM(deci2addr);

			if (d2ptr[1] > 0xc)
			{
				// this looks horribly wrong, justification please?
				uint8_t * pdeciaddr = (uint8_t*)dmaGetAddr(d2ptr[4]+0xc, false);
				if(pdeciaddr)
					pdeciaddr += (d2ptr[4]+0xc) % 16;
				else
					pdeciaddr = (u8*)PSM(d2ptr[4]+0xc);

				const int copylen = std::min<unsigned int>(255, d2ptr[1]-0xc);
				memcpy(deci2buffer, pdeciaddr, copylen );
				deci2buffer[copylen] = '\0';
			}
			((u32*)PSM(deci2addr))[3] = 0;
			return 1;
		}

		case 4: /* poll */
		case 5: /* exrecv */
		case 6: /* exsend */
		case 0x10: /* kputs */
			return 1;
	}

	return 0;
}

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

void COP2(void)         { Int_COP2PrintTable[_Rs_](); }
void Unknown(void)      { }
void MMI_Unknown(void)  { }
void COP0_Unknown(void) { }
void COP1_Unknown(void) { }

/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/

// Implementation Notes:
//  * It is important that instructions perform overflow checks prior to shortcutting on
//    the zero register (when it is used as a destination).  Overflow exceptions are still
//    handled even though the result is discarded.

// Rt = Rs + Im signed [exception on overflow]
void ADDI(void)
{
	int64_t result = 0;
	bool overflow  = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, &result);
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

// Rt = Rs + Im signed !!! [overflow ignored]
// This instruction is effectively identical to ADDI.  It is not a true unsigned operation,
// but rather it is a signed operation that ignores overflows.
void ADDIU(void)
{
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = u64(int64_t(int32_t(cpuRegs.GPR.r[_Rs_].UL[0] + u32(int32_t(_Imm_)))));
}

// Rt = Rs + Im [exception on overflow]
// This is the full 64 bit version of ADDI.  Overflow occurs at 64 bits instead
// of at 32 bits.
void DADDI(void)
{
	int64_t result = 0;
	bool overflow  = _add64_Overflow(cpuRegs.GPR.r[_Rs_].SD[0], _Imm_, &result );
	if (overflow || !_Rt_) return;
	cpuRegs.GPR.r[_Rt_].SD[0] = result;
}

// Rt = Rs + Im [overflow ignored]
// This instruction is effectively identical to DADDI.  It is not a true unsigned operation,
// but rather it is a signed operation that ignores overflows.
void DADDIU(void)
{
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0] + u64(int64_t(_Imm_));
}
void ANDI(void)     { if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  & (uint64_t)_ImmU_; } // Rt = Rs And Im (zero-extended)
void ORI(void) 	    { if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  | (uint64_t)_ImmU_; } // Rt = Rs Or  Im (zero-extended)
void XORI(void)	    { if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  ^ (uint64_t)_ImmU_; } // Rt = Rs Xor Im (zero-extended)
void SLTI(void)     { if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < (int64_t)(_Imm_))  ? 1 : 0; } // Rt = Rs < Im (signed)
void SLTIU(void)    { if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < (uint64_t)(_Imm_)) ? 1 : 0; } // Rt = Rs < Im (unsigned)

/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

// Rd = Rs + Rt		(Exception on Integer Overflow)
void ADD()
{
	int64_t result = 0;
	bool overflow  = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], &result);
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void DADD(void)
{
	int64_t result = 0;
	bool overflow  = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], cpuRegs.GPR.r[_Rt_].SD[0], &result);
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void SUB(void)
{
	int64_t result = 0;
	bool overflow  = _add32_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], &result );
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void DSUB(void)
{
	int64_t result = 0;
	bool overflow  = _add64_Overflow( cpuRegs.GPR.r[_Rs_].SD[0], -cpuRegs.GPR.r[_Rt_].SD[0], &result);
	if (overflow || !_Rd_) return;
	cpuRegs.GPR.r[_Rd_].SD[0] = result;
}

void ADDU(void)	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(int64_t)(int32_t)(cpuRegs.GPR.r[_Rs_].UL[0]  + cpuRegs.GPR.r[_Rt_].UL[0]); }	// Rd = Rs + Rt
void DADDU(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  + cpuRegs.GPR.r[_Rt_].UD[0]; }
void SUBU(void)	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(int64_t)(int32_t)(cpuRegs.GPR.r[_Rs_].UL[0]  - cpuRegs.GPR.r[_Rt_].UL[0]); }	// Rd = Rs - Rt
void DSUBU(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  - cpuRegs.GPR.r[_Rt_].UD[0]; }
void AND(void) 	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  & cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs And Rt
void OR(void) 	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  | cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs Or  Rt
void XOR(void) 	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]  ^ cpuRegs.GPR.r[_Rt_].UD[0]; }	// Rd = Rs Xor Rt
void NOR(void) 	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] =~(cpuRegs.GPR.r[_Rs_].UD[0] | cpuRegs.GPR.r[_Rt_].UD[0]); }// Rd = Rs Nor Rt
void SLT(void)	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].SD[0] < cpuRegs.GPR.r[_Rt_].SD[0]) ? 1 : 0; }	// Rd = Rs < Rt (signed)
void SLTU(void)	{ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (cpuRegs.GPR.r[_Rs_].UD[0] < cpuRegs.GPR.r[_Rt_].UD[0]) ? 1 : 0; }	// Rd = Rs < Rt (unsigned)

/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/

// Signed division "overflows" on (0x80000000 / -1), here (LO = 0x80000000, HI = 0) is returned by MIPS
// in division by zero on MIPS, it appears that:
// LO gets 1 if rs is negative (and the division is signed) and -1 otherwise.
// HI gets the value of rs.

// Result is stored in HI/LO [no arithmetic exceptions]
void DIV(void)
{
	if (cpuRegs.GPR.r[_Rs_].UL[0] == 0x80000000 && cpuRegs.GPR.r[_Rt_].UL[0] == 0xffffffff)
	{
		cpuRegs.LO.SD[0] = (int32_t)0x80000000;
		cpuRegs.HI.SD[0] = (int32_t)0x0;
	}
	else if (cpuRegs.GPR.r[_Rt_].SL[0] != 0)
	{
		cpuRegs.LO.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] / cpuRegs.GPR.r[_Rt_].SL[0];
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0] % cpuRegs.GPR.r[_Rt_].SL[0];
	}
	else
	{
		cpuRegs.LO.SD[0] = (cpuRegs.GPR.r[_Rs_].SL[0] < 0) ? 1 : -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

// Result is stored in HI/LO [no arithmetic exceptions]
void DIVU(void)
{
	if (cpuRegs.GPR.r[_Rt_].UL[0] != 0)
	{
		// note: DIVU has no sign extension when assigning back to 64 bits
		// note 2: reference material strongly disagrees. (air)
		cpuRegs.LO.SD[0] = (int32_t)(cpuRegs.GPR.r[_Rs_].UL[0] / cpuRegs.GPR.r[_Rt_].UL[0]);
		cpuRegs.HI.SD[0] = (int32_t)(cpuRegs.GPR.r[_Rs_].UL[0] % cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else
	{
		cpuRegs.LO.SD[0] = -1;
		cpuRegs.HI.SD[0] = cpuRegs.GPR.r[_Rs_].SL[0];
	}
}

// Result is written to both HI/LO and to the _Rd_ (Lo only)
void MULT(void)
{
	int64_t res = (int64_t)cpuRegs.GPR.r[_Rs_].SL[0] * cpuRegs.GPR.r[_Rt_].SL[0];

	// Sign-extend into 64 bits:
	cpuRegs.LO.SD[0] = (int32_t)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (int32_t)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

// Result is written to both HI/LO and to the _Rd_ (Lo only)
void MULTU(void)
{
	uint64_t res = (uint64_t)cpuRegs.GPR.r[_Rs_].UL[0] * cpuRegs.GPR.r[_Rt_].UL[0];

	// Note: sign-extend into 64 bits even though it's an unsigned mult.
	cpuRegs.LO.SD[0] = (int32_t)(res & 0xffffffff);
	cpuRegs.HI.SD[0] = (int32_t)(res >> 32);

	if( _Rd_ ) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0];
}

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/
void LUI(void) {
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = (int32_t)(cpuRegs.code << 16);
}

/*********************************************************
* Move from HI/LO to GPR                                 *
* Format:  OP rd                                         *
*********************************************************/
void MFHI(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.HI.UD[0]; } // Rd = Hi
void MFLO(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.LO.UD[0]; } // Rd = Lo

/*********************************************************
* Move to GPR to HI/LO & Register jump                   *
* Format:  OP rs                                         *
*********************************************************/
void MTHI(void) { cpuRegs.HI.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; } // Hi = Rs
void MTLO(void) { cpuRegs.LO.UD[0] = cpuRegs.GPR.r[_Rs_].UD[0]; } // Lo = Rs


/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
void SRA(void)   { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].SL[0] >> _Sa_); } // Rd = Rt >> sa (arithmetic)
void SRL(void)   { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].UL[0] >> _Sa_); } // Rd = Rt >> sa (logical) [sign extend!!]
void SLL(void)   { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].UL[0] << _Sa_); } // Rd = Rt << sa
void DSLL(void)  { if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(cpuRegs.GPR.r[_Rt_].UD[0] << _Sa_); }
void DSLL32(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(cpuRegs.GPR.r[_Rt_].UD[0] << (_Sa_+32));}
void DSRA(void)  { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> _Sa_; }
void DSRA32(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = cpuRegs.GPR.r[_Rt_].SD[0] >> (_Sa_+32);}
void DSRL(void)  { if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> _Sa_; }
void DSRL32(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rt_].UD[0] >> (_Sa_+32);}

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/
void SLLV(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].UL[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt << rs
void SRAV(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].SL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt >> rs (arithmetic)
void SRLV(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int32_t) (cpuRegs.GPR.r[_Rt_].UL[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x1f));} // Rd = Rt >> rs (logical)
void DSLLV(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(cpuRegs.GPR.r[_Rt_].UD[0] << (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRAV(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].SD[0] = (int64_t) (cpuRegs.GPR.r[_Rt_].SD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}
void DSRLV(void){ if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (uint64_t)(cpuRegs.GPR.r[_Rt_].UD[0] >> (cpuRegs.GPR.r[_Rs_].UL[0] &0x3f));}

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/

// Implementation Notes Regarding Memory Operations:
//  * It it 'correct' to do all loads into temp variables, even if the destination GPR
//    is the zero reg (which nullifies the result).  The memory needs to be accessed
//    regardless so that hardware registers behave as expected (some clear on read) and
//    so that TLB Misses are handled as expected as well.
//
//  * Low/High varieties of instructions, such as LWL/LWH, do *not* raise Address Error
//    exceptions, since the lower bits of the address are used to determine the portions
//    of the address/register operations.

void LB(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	int8_t temp   = vtlb_memRead8(addr);
	if (_Rt_) cpuRegs.GPR.r[_Rt_].SD[0] = temp;
}

void LBU(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	uint8_t temp  = vtlb_memRead8(addr);
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

void LH(void)
{
	int16_t temp;
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 1))
		Cpu->CancelInstruction();

	temp = vtlb_memRead16(addr);
	if (_Rt_) cpuRegs.GPR.r[_Rt_].SD[0] = temp;
}

void LHU(void)
{
	uint16_t temp;
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 1))
		Cpu->CancelInstruction();

	temp = vtlb_memRead16(addr);
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

void LW(void)
{
	uint32_t temp;
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 3))
		Cpu->CancelInstruction();

	temp = vtlb_memRead32(addr);

	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].SD[0] = (int32_t)temp;
}

void LWU(void)
{
	uint32_t temp;
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 3))
		Cpu->CancelInstruction();

	temp = vtlb_memRead32(addr);

	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].UD[0] = temp;
}

void LWL(void)
{
	static const uint32_t LWL_MASK[4] = { 0xffffff, 0x0000ffff, 0x000000ff, 0x00000000 };
	static const uint8_t LWL_SHIFT[4] = { 24, 16, 8, 0 };
	int32_t addr                      = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	uint32_t shift                    = addr & 3;
	uint32_t mem                      = vtlb_memRead32(addr & ~3);

	/* ensure the compiler does correct sign extension into 64 bits by using int32_t */
	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].SD[0] =	(int32_t)((cpuRegs.GPR.r[_Rt_].UL[0] & LWL_MASK[shift])
				              | (mem << LWL_SHIFT[shift]));

	/*
	Mem = 1234.  Reg = abcd
	(result is always sign extended into the upper 32 bits of the Rt)

	0   4bcd   (mem << 24) | (reg & 0x00ffffff)
	1   34cd   (mem << 16) | (reg & 0x0000ffff)
	2   234d   (mem <<  8) | (reg & 0x000000ff)
	3   1234   (mem      ) | (reg & 0x00000000)
	*/
}

void LWR(void)
{
	static const u32 LWR_MASK[4] = { 0x000000, 0xff000000, 0xffff0000, 0xffffff00 };
	static const u8 LWR_SHIFT[4] = { 0, 8, 16, 24 };
	int32_t addr   = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	uint32_t shift = addr & 3;
	uint32_t mem   = vtlb_memRead32(addr & ~3);

	if (!_Rt_) return;

	// Use unsigned math here, and conditionally sign extend below, when needed.
	mem = (cpuRegs.GPR.r[_Rt_].UL[0] & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);

	// This special case requires sign extension into the full 64 bit dest.
	if (shift == 0)
		cpuRegs.GPR.r[_Rt_].SD[0] =	(int32_t)mem;
	else
		// This case sets the lower 32 bits of the target register.  Upper
		// 32 bits are always preserved.
		cpuRegs.GPR.r[_Rt_].UL[0] =	mem;

	/*
	Mem = 1234.  Reg = abcd

	0   1234   (mem      ) | (reg & 0x00000000)	[sign extend into upper 32 bits!]
	1   a123   (mem >>  8) | (reg & 0xff000000)
	2   ab12   (mem >> 16) | (reg & 0xffff0000)
	3   abc1   (mem >> 24) | (reg & 0xffffff00)
	*/
}

// dummy variable used as a destination address for writes to the zero register, so
// that the zero register always stays zero.
alignas(16) static GPR_reg m_dummy_gpr_zero;

// Returns the x86 address of the requested GPR, which is safe for writing. (includes
// special handling for returning a dummy var for GPR0(zero), so that it's value is
// always preserved)
#define GPR_GETWRITEPTR(gpr) (( gpr == 0 ) ? &m_dummy_gpr_zero : &cpuRegs.GPR.r[gpr])

void LD(void)
{
	int32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 7))
		Cpu->CancelInstruction();

	cpuRegs.GPR.r[_Rt_].UD[0] = vtlb_memRead64(addr);
}


void LDL(void)
{
	static const u64 LDL_MASK[8] =
	{	0x00ffffffffffffffULL, 0x0000ffffffffffffULL, 0x000000ffffffffffULL, 0x00000000ffffffffULL,
		0x0000000000ffffffULL, 0x000000000000ffffULL, 0x00000000000000ffULL, 0x0000000000000000ULL
	};
	static const u8 LDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	uint32_t addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	uint32_t shift = addr & 7;
	uint64_t mem   = vtlb_memRead64(addr & ~7);
	if(_Rt_ )
		cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDL_MASK[shift]) |
			(mem << LDL_SHIFT[shift]);
}

void LDR(void)
{
	static const u64 LDR_MASK[8] =
	{	0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
		0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL
	};
	static const u8 LDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	uint32_t addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	uint32_t shift = addr & 7;
	uint64_t mem   = vtlb_memRead64(addr & ~7);
	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDR_MASK[shift]) |
			(mem >> LDR_SHIFT[shift]);
}

void LQ(void)
{
	/* MIPS Note: LQ and SQ are special and "silently" align memory addresses, thus
	 * an address error due to unaligned access isn't possible like it is on other loads/stores. */
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memRead128(addr & ~0xf, (u128*)GPR_GETWRITEPTR(_Rt_));
}

void SB(void)
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	vtlb_memWrite8(addr, cpuRegs.GPR.r[_Rt_].UC[0]);
}

void SH(void)
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 1))
		Cpu->CancelInstruction();

	vtlb_memWrite16(addr, cpuRegs.GPR.r[_Rt_].US[0]);
}

void SW(void)
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 3))
		Cpu->CancelInstruction();

	vtlb_memWrite32(addr, cpuRegs.GPR.r[_Rt_].UL[0]);
}

void SWL(void)
{
	static const u32 SWL_MASK[4] = { 0xffffff00, 0xffff0000, 0xff000000, 0x00000000 };
	static const u8 SWL_SHIFT[4] = { 24, 16, 8, 0 };
	u32 addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem   = vtlb_memRead32( addr & ~3 );
	vtlb_memWrite32( addr & ~3,
		  (cpuRegs.GPR.r[_Rt_].UL[0] >> SWL_SHIFT[shift])
		| (mem & SWL_MASK[shift])
	);

	/*
	Mem = 1234.  Reg = abcd

	0   123a   (reg >> 24) | (mem & 0xffffff00)
	1   12ab   (reg >> 16) | (mem & 0xffff0000)
	2   1abc   (reg >>  8) | (mem & 0xff000000)
	3   abcd   (reg      ) | (mem & 0x00000000)
	*/
}

void SWR(void)
{
	static const u32 SWR_MASK[4] = { 0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };
	static const u8 SWR_SHIFT[4] = { 0, 8, 16, 24 };
	u32 addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 3;
	u32 mem   = vtlb_memRead32(addr & ~3);
	vtlb_memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] << SWR_SHIFT[shift]) |
		(mem & SWR_MASK[shift])
	);

	/*
	Mem = 1234.  Reg = abcd

	0   abcd   (reg      ) | (mem & 0x00000000)
	1   bcd4   (reg <<  8) | (mem & 0x000000ff)
	2   cd34   (reg << 16) | (mem & 0x0000ffff)
	3   d234   (reg << 24) | (mem & 0x00ffffff)
	*/
}

void SD()
{
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;

	if (unlikely(addr & 7))
		Cpu->CancelInstruction();

	vtlb_memWrite64(addr,cpuRegs.GPR.r[_Rt_].UD[0]);
}


void SDL()
{
	static const u64 SDL_MASK[8] =
	{	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
		0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL
	};
	static const u8 SDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	u32 addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem   = vtlb_memRead64(addr & ~7);
	mem       = (cpuRegs.GPR.r[_Rt_].UD[0] >> SDL_SHIFT[shift]) |
		    (mem & SDL_MASK[shift]);
	vtlb_memWrite64(addr & ~7, mem);
}


void SDR()
{
	static const u64 SDR_MASK[8] =
	{	0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
		0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL
	};
	static const u8 SDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	u32 addr  = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	u32 shift = addr & 7;
	u64 mem   = vtlb_memRead64(addr & ~7);
	mem       = (cpuRegs.GPR.r[_Rt_].UD[0] << SDR_SHIFT[shift]) |
		    (mem & SDR_MASK[shift]);
	vtlb_memWrite64(addr & ~7, mem );
}

void SQ(void)
{
	/* MIPS Note: LQ and SQ are special and "silently" align memory addresses, thus
	 * an address error due to unaligned access isn't possible like it is on other loads/stores. */
	u32 addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	memWrite128(addr & ~0xf, &cpuRegs.GPR.r[_Rt_].UQ);
}

/*********************************************************
* Conditional Move                                       *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

void MOVZ(void)
{
	if (_Rd_ && cpuRegs.GPR.r[_Rt_].UD[0] == 0)
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
}

void MOVN(void)
{
	if (_Rd_ && cpuRegs.GPR.r[_Rt_].UD[0] != 0)
		cpuRegs.GPR.r[_Rd_].UD[0] = cpuRegs.GPR.r[_Rs_].UD[0];
}

/*********************************************************
* Special purpose instructions                           *
* Format:  OP                                            *
*********************************************************/

void SYSCALL(void)
{
	uint8_t call;

	if (cpuRegs.GPR.n.v1.SL[0] < 0)
		call = (u8)(-cpuRegs.GPR.n.v1.SL[0]);
	else
		call = cpuRegs.GPR.n.v1.UC[0];

	switch (static_cast<Syscall>(call))
	{
		case Syscall::SetGsCrt:
		{
			/* Function "SetGsCrt(Interlace, Mode, Field)"
			 * Useful for fetching information of interlace/video/field display parameters of the Graphics Synthesizer */

			// (cpuRegs.GPR.n.a2.UL[0] & 1) == GS is frame mode
			//  Warning info might be incorrect!
			switch (cpuRegs.GPR.n.a1.UC[0])
			{
				case 0x0: /* "NTSC 640x448 @ 59.940 (59.82)" */
				case 0x2: /* "NTSC 640x448 @ 59.940 (59.82)" */
					gsSetVideoMode(GS_VideoMode::NTSC);
					break;
				case 0x1: /* "PAL  640x512 @ 50.000 (49.76)" */
				case 0x3: /* "PAL  640x512 @ 50.000 (49.76)" */
					gsSetVideoMode(GS_VideoMode::PAL);
					break;
				case 0x1A: /* "VESA 640x480 @ 59.940" */
				case 0x1B: /* "VESA 640x480 @ 72.809" */
				case 0x1C: /* "VESA 640x480 @ 75.000" */
				case 0x1D: /* "VESA 640x480 @ 85.008" */
				case 0x2A: /* "VESA 800x600 @ 56.250" */
				case 0x2B: /* "VESA 800x600 @ 60.317" */
				case 0x2C: /* "VESA 800x600 @ 72.188" */
				case 0x2D: /* "VESA 800x600 @ 75.000" */
				case 0x2E: /* "VESA 800x600 @ 85.061" */
				case 0x3B: /* "VESA 1024x768 @ 60.004" */ 
				case 0x3C: /* "VESA 1024x768 @ 70.069" */
				case 0x3D: /* "VESA 1024x768 @ 75.029" */
				case 0x3E: /* "VESA 1024x768 @ 84.997" */
				case 0x4A: /* "VESA 1280x1024 @ 63.981" */
				case 0x4B: /* "VESA 1280x1024 @ 79.976" */
					gsSetVideoMode(GS_VideoMode::VESA); break;
				case 0x50: /* "SDTV   720x480 @ 59.94"  */
					gsSetVideoMode(GS_VideoMode::SDTV_480P);
					break;
				case 0x51: /* "HDTV 1920x1080 @ 60.00"  */
					gsSetVideoMode(GS_VideoMode::HDTV_1080I);
					break;
				case 0x52: /* "HDTV  1280x720 @ ??.???" */
					gsSetVideoMode(GS_VideoMode::HDTV_720P);
					break;
				case 0x53: /* "SDTV   768x576 @ ??.???" */
					gsSetVideoMode(GS_VideoMode::SDTV_576P);
					break;
				case 0x54: /* "HDTV 1920x1080 @ ??.???" */
					gsSetVideoMode(GS_VideoMode::HDTV_1080P);
					break;
				case 0x72: /* "DVD NTSC 640x448 @ ??.???" */
				case 0x82: /* "DVD NTSC 640x448 @ ??.???" */
					gsSetVideoMode(GS_VideoMode::DVD_NTSC);
					break;
				case 0x73: /* "DVD PAL 720x480 @ ??.???" */
				case 0x83: /* "DVD PAL 720x480 @ ??.???" */
					gsSetVideoMode(GS_VideoMode::DVD_PAL);
					break;

				default:
					gsSetVideoMode(GS_VideoMode::Unknown);
			}
		}
		break;
		case Syscall::SetOsdConfigParam:
			AllowParams1 = true;
			break;
		case Syscall::GetOsdConfigParam:
			if(!NoOSD && g_SkipBiosHack && !AllowParams1)
			{
				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u8 params[16];

				cdvdReadLanguageParams(params);

				u32 timezone = params[4] | ((u32)(params[3] & 0x7) << 8);
				u32 osdconf  = params[1] & 0x1F;			// SPDIF, Screen mode, RGB/Comp, Jap/Eng Switch (Early bios)
				osdconf |= (u32)params[0] << 5;				// PS1 Mode Settings
				osdconf |= (u32)((params[2] & 0xE0) >> 5) << 13;	// OSD Ver (Not sure but best guess)
				osdconf |= (u32)(params[2] & 0x1F) << 16;		// Language
				osdconf |= timezone << 21;				// Timezone

				vtlb_memWrite32(memaddr, osdconf);
				return;
			}
			break;
		case Syscall::SetOsdConfigParam2:
			AllowParams2 = true;
			break;
		case Syscall::GetOsdConfigParam2:
			if (!NoOSD && g_SkipBiosHack && !AllowParams2)
			{
				u32 memaddr = cpuRegs.GPR.n.a0.UL[0];
				u8 params[16];

				cdvdReadLanguageParams(params);

				u32 osdconf2 = (((u32)params[3] & 0x78) << 9);  // Daylight Savings, 24hr clock, Date format

				vtlb_memWrite32(memaddr, osdconf2);
				return;
			}
			break;
		case Syscall::ExecPS2:
		case Syscall::sceSifSetDma:
		case Syscall::SetVTLBRefillHandler:
			break;
		case Syscall::StartThread:
		case Syscall::ChangeThreadPriority:
		{
			if (CurrentBiosInformation.eeThreadListAddr == 0)
			{
				uint32_t offset = 0x0;
				/* Surprisingly not that slow :) */
				while (offset < 0x5000) /* I find that the instructions are in between 0x4000 -> 0x5000 */
				{
					uint32_t        addr = 0x80000000 + offset;
					const uint32_t inst1 = vtlb_memRead32(addr);
					const uint32_t inst2 = vtlb_memRead32(addr += 4);
					const uint32_t inst3 = vtlb_memRead32(addr += 4);

					if (       ThreadListInstructions[0] == inst1  /* sw v0,0x0(v0) */
						&& ThreadListInstructions[1] == inst2  /* no-op */
						&& ThreadListInstructions[2] == inst3) /* no-op */
					{
						// We've found the instruction pattern!
						// We (well, I) know that the thread address is always 0x8001 + the immediate of the 6th instruction from here
						const uint32_t op = vtlb_memRead32(0x80000000 + offset + (sizeof(u32) * 6));
						CurrentBiosInformation.eeThreadListAddr = 0x80010000 + static_cast<u16>(op) - 8; // Subtract 8 because the address here is offset by 8.
						break;
					}
					offset += 4;
				}
				/* We couldn't find the address */
				if (!CurrentBiosInformation.eeThreadListAddr)
					CurrentBiosInformation.eeThreadListAddr = -1;
			}
		}
		break;
		case Syscall::Deci2Call:
		{
			if (cpuRegs.GPR.n.a0.UL[0] != 0x10)
				__Deci2Call(cpuRegs.GPR.n.a0.UL[0], (u32*)PSM(cpuRegs.GPR.n.a1.UL[0]));

			break;
		}
		case Syscall::sysPrintOut:
		{
			if (cpuRegs.GPR.n.a0.UL[0] != 0)
			{
				int i, curRegArg = 0;
				// TODO: Only supports 7 format arguments. Need to read from the stack for more.
				// Is there a function which collects PS2 arguments?
				char* fmt = (char*)PSM(cpuRegs.GPR.n.a0.UL[0]);

				u64 regs[7] = {
					cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0],
					cpuRegs.GPR.n.a3.UL[0],
					cpuRegs.GPR.n.t0.UL[0],
					cpuRegs.GPR.n.t1.UL[0],
					cpuRegs.GPR.n.t2.UL[0],
					cpuRegs.GPR.n.t3.UL[0],
				};

				// Pretty much what this does is find instances of string arguments and remaps them.
				// Instead of the addresse(s) being relative to the PS2 address space, make them relative to program memory.
				// (This fixes issue #2865)
				for (i = 0; 1; i++)
				{
					if (fmt[i] == '\0')
						break;

					if (fmt[i] == '%')
					{
						// The extra check here is to be compatible with "%%s"
						if (i == 0 || fmt[i - 1] != '%') {
							if (fmt[i + 1] == 's')
								regs[curRegArg] = (u64)PSM(regs[curRegArg]); // PS2 Address -> PCSX2 Address
							curRegArg++;
						}
					}
				}
			}
			break;
		}


		default:
			break;
	}

	cpuRegs.pc -= 4;
	cpuException(0x20, cpuRegs.branch);
}

void BREAK(void)
{
	cpuRegs.pc -= 4;
	cpuException(0x24, cpuRegs.branch);
}

void MFSA(void) { if (_Rd_) cpuRegs.GPR.r[_Rd_].UD[0] = (u64)cpuRegs.sa; }
void MTSA(void) { cpuRegs.sa = (u32)cpuRegs.GPR.r[_Rs_].UD[0]; }

/* SNY supports three basic modes, two which synchronize memory accesses (related
 * to the cache) and one which synchronizes the instruction pipeline (effectively
 * a stall in either case).  Our emulation model does not track EE-side pipeline
 * status or stalls, nor does it implement the CACHE.  Thus SYNC need do nothing. */
void SYNC(void) { }
/* Used to prefetch data into the EE's cache, or schedule a dirty write-back.
 * CACHE is not emulated at this time (nor is there any need to emulate it), so
 * this function does nothing in the context of our emulator. */
void PREF(void) { }

#define TRAP() cpuRegs.pc -= 4; cpuException(0x34, cpuRegs.branch)

/*********************************************************
* Register trap                                          *
* Format:  OP rs, rt                                     *
*********************************************************/
void TGE(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= cpuRegs.GPR.r[_Rt_].SD[0]) { TRAP(); } }
void TGEU(void) { if (cpuRegs.GPR.r[_Rs_].UD[0] >= cpuRegs.GPR.r[_Rt_].UD[0]) { TRAP(); } }
void TLT(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  cpuRegs.GPR.r[_Rt_].SD[0]) { TRAP(); } }
void TLTU(void) { if (cpuRegs.GPR.r[_Rs_].UD[0] <  cpuRegs.GPR.r[_Rt_].UD[0]) { TRAP(); } }
void TEQ(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0]) { TRAP(); } }
void TNE(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0]) { TRAP(); } }

/*********************************************************
* Trap with immediate operand                            *
* Format:  OP rs, rt                                     *
*********************************************************/
void TGEI(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] >= _Imm_) { TRAP(); } }
void TLTI(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] <  _Imm_) { TRAP(); } }
void TEQI(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] == _Imm_) { TRAP(); } }
void TNEI(void)  { if (cpuRegs.GPR.r[_Rs_].SD[0] != _Imm_) { TRAP(); } }
void TGEIU(void) { if (cpuRegs.GPR.r[_Rs_].UD[0] >= (uint64_t)_Imm_) { TRAP(); } }
void TLTIU(void) { if (cpuRegs.GPR.r[_Rs_].UD[0] <  (uint64_t)_Imm_) { TRAP(); } }

/*********************************************************
* Sa intructions                                         *
* Format:  OP rs, rt                                     *
*********************************************************/

void MTSAB(void) { cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)); }
void MTSAH(void) { cpuRegs.sa = ((cpuRegs.GPR.r[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1; }

} }	} // end namespace R5900::Interpreter::OpcodeImpl
