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


#include "R3000A.h"
#include "IopGte.h"
#include "IopMem.h"

/* Note: Branch instructions of the Interpreter are defined externally because
 * the recompiler shouldn't be using them (it isn't entirely safe, due to the
 * delay slot and event handling differences between recs and ints) */

/* Prototypes */
void psxBGEZ(void);
void psxBGEZAL(void);
void psxBGTZ(void);
void psxBLEZ(void);
void psxBLTZ(void);
void psxBLTZAL(void);

void psxBEQ(void);
void psxBNE(void);
void psxJ(void);
void psxJAL(void);

void psxJR(void);
void psxJALR(void);

/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/
void psxADDI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) + _Imm_ ; /* Rt = Rs + Im (Exception on Integer Overflow) */ } 
void psxADDIU(void)  { if (_Rt_) _rRt_ = _u32(_rRs_) + _Imm_ ; /* Rt = Rs + Im */ }
void psxANDI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) & _ImmU_; }		// Rt = Rs And Im
void psxORI(void)    { if (_Rt_) _rRt_ = _u32(_rRs_) | _ImmU_; }		// Rt = Rs Or  Im
void psxXORI(void)   { if (_Rt_) _rRt_ = _u32(_rRs_) ^ _ImmU_; }		// Rt = Rs Xor Im
void psxSLTI(void)   { if (_Rt_) _rRt_ = _i32(_rRs_) < _Imm_ ; }		// Rt = Rs < Im	(Signed)
void psxSLTIU(void)  { if (_Rt_) _rRt_ = _u32(_rRs_) < (u32)_Imm_; }		// Rt = Rs < Im	(Unsigned)

/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/
void psxADD(void)	{ if (_Rd_) _rRd_ = _u32(_rRs_) + _u32(_rRt_);   }	// Rd = Rs + Rt		(Exception on Integer Overflow)
void psxADDU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) + _u32(_rRt_);   }	// Rd = Rs + Rt
void psxSUB(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) - _u32(_rRt_);   }	// Rd = Rs - Rt		(Exception on Integer Overflow)
void psxSUBU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) - _u32(_rRt_);   }	// Rd = Rs - Rt
void psxAND(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) & _u32(_rRt_);   }	// Rd = Rs And Rt
void psxOR(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) | _u32(_rRt_);   }	// Rd = Rs Or  Rt
void psxXOR(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) ^ _u32(_rRt_);   }	// Rd = Rs Xor Rt
void psxNOR(void) 	{ if (_Rd_) _rRd_ =~(_u32(_rRs_) | _u32(_rRt_)); }	// Rd = Rs Nor Rt
void psxSLT(void) 	{ if (_Rd_) _rRd_ = _i32(_rRs_) < _i32(_rRt_);   }	// Rd = Rs < Rt		(Signed)
void psxSLTU(void) 	{ if (_Rd_) _rRd_ = _u32(_rRs_) < _u32(_rRt_);   }	// Rd = Rs < Rt		(Unsigned)

/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/
void psxDIV(void)
{
	if (_rRt_ == 0) {
		// Division by 0
		_rLo_ = _i32(_rRs_) < 0 ? 1 : 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else if (_rRs_ == 0x80000000u && _rRt_ == 0xFFFFFFFFu) {
		// x86 overflow
		_rLo_ = 0x80000000u;
		_rHi_ = 0;

	} else {
		// Normal behavior
		_rLo_ = _i32(_rRs_) / _i32(_rRt_);
		_rHi_ = _i32(_rRs_) % _i32(_rRt_);
	}
}

void psxDIVU(void)
{
	if (_rRt_ == 0) {
		// Division by 0
		_rLo_ = 0xFFFFFFFFu;
		_rHi_ = _rRs_;

	} else {
		// Normal behavior
		_rLo_ = _rRs_ / _rRt_;
		_rHi_ = _rRs_ % _rRt_;
	}
}

void psxMULT(void)
{
	uint64_t res     = (int64_t)((int64_t)_i32(_rRs_) * (int64_t)_i32(_rRt_));
	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

void psxMULTU(void)
{
	uint64_t res = (uint64_t)((uint64_t)_u32(_rRs_) * (uint64_t)_u32(_rRt_));

	psxRegs.GPR.n.lo = (u32)(res & 0xffffffff);
	psxRegs.GPR.n.hi = (u32)((res >> 32) & 0xffffffff);
}

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
void psxSLL(void) { if (_Rd_) _rRd_ = _u32(_rRt_) << _Sa_; } // Rd = Rt << sa
void psxSRA(void) { if (_Rd_) _rRd_ = _i32(_rRt_) >> _Sa_; } // Rd = Rt >> sa (arithmetic)
void psxSRL(void) { if (_Rd_) _rRd_ = _u32(_rRt_) >> _Sa_; } // Rd = Rt >> sa (logical)

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/
void psxSLLV(void) { if (_Rd_) _rRd_ = _u32(_rRt_) << _u32(_rRs_); } // Rd = Rt << rs
void psxSRAV(void) { if (_Rd_) _rRd_ = _i32(_rRt_) >> _u32(_rRs_); } // Rd = Rt >> rs (arithmetic)
void psxSRLV(void) { if (_Rd_) _rRd_ = _u32(_rRt_) >> _u32(_rRs_); } // Rd = Rt >> rs (logical)

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/
void psxLUI(void) { if (_Rt_) _rRt_ = psxRegs.code << 16; } // Upper halfword of Rt = Im

/*********************************************************
* Move from HI/LO to GPR                                 *
* Format:  OP rd                                         *
*********************************************************/
void psxMFHI(void) { if (_Rd_) _rRd_ = _rHi_; } // Rd = Hi
void psxMFLO(void) { if (_Rd_) _rRd_ = _rLo_; } // Rd = Lo

/*********************************************************
* Move to GPR to HI/LO & Register jump                   *
* Format:  OP rs                                         *
*********************************************************/
void psxMTHI(void) { _rHi_ = _rRs_; } // Hi = Rs
void psxMTLO(void) { _rLo_ = _rRs_; } // Lo = Rs

/*********************************************************
* Special purpose instructions                           *
* Format:  OP                                            *
*********************************************************/
void psxBREAK(void)
{
	/* Break exception - PSX ROM doens't handle this */
	psxRegs.pc -= 4;
	psxException(0x24, iopIsDelaySlot);
}

void psxSYSCALL(void)
{
	psxRegs.pc -= 4;
	psxException(0x20, iopIsDelaySlot);

}

void psxRFE(void)
{
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status & 0xfffffff0)
		            | ((psxRegs.CP0.n.Status & 0x3c) >> 2);
}

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/

#define _oB_ (_u32(_rRs_) + _Imm_)

void psxLB(void)
{
	if (_Rt_)
		_rRt_ = (s8 )iopMemRead8(_oB_);
	else
	{
		iopMemRead8(_oB_);
	}
}

void psxLBU() {
	if (_Rt_) {
		_rRt_ = iopMemRead8(_oB_);
	} else {
		iopMemRead8(_oB_);
	}
}

void psxLH(void)
{
	if (_Rt_)
		_rRt_ = (s16)iopMemRead16(_oB_);
	else
	{
		iopMemRead16(_oB_);
	}
}

void psxLHU(void)
{
	if (_Rt_)
		_rRt_ = iopMemRead16(_oB_);
	else
	{
		iopMemRead16(_oB_);
	}
}

void psxLW(void)
{
	if (_Rt_)
		_rRt_ = iopMemRead32(_oB_);
	else
	{
		iopMemRead32(_oB_);
	}
}

void psxLWL(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	if (_Rt_)
		_rRt_ =	  ( _u32(_rRt_) & (0x00ffffff >> shift) )
			| ( mem << (24 - shift) );

	/*
	Mem = 1234.  Reg = abcd

	0   4bcd   (mem << 24) | (reg & 0x00ffffff)
	1   34cd   (mem << 16) | (reg & 0x0000ffff)
	2   234d   (mem <<  8) | (reg & 0x000000ff)
	3   1234   (mem      ) | (reg & 0x00000000)

	*/
}

void psxLWR(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	if (_Rt_)
		_rRt_ =	  ( _u32(_rRt_) & (0xffffff00 << (24 - shift)) )
			| ( mem  >> shift );

	/*
	Mem = 1234.  Reg = abcd

	0   1234   (mem      ) | (reg & 0x00000000)
	1   a123   (mem >>  8) | (reg & 0xff000000)
	2   ab12   (mem >> 16) | (reg & 0xffff0000)
	3   abc1   (mem >> 24) | (reg & 0xffffff00)

	*/
}

void psxSB(void) { iopMemWrite8 (_oB_, _u8 (_rRt_)); }
void psxSH(void) { iopMemWrite16(_oB_, _u16(_rRt_)); }
void psxSW(void) { iopMemWrite32(_oB_, _u32(_rRt_)); }

void psxSWL(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc),  ( ( _u32(_rRt_) >>  (24 - shift) ) ) |
			     (  mem & (0xffffff00 << shift) ));
	/*
	Mem = 1234.  Reg = abcd

	0   123a   (reg >> 24) | (mem & 0xffffff00)
	1   12ab   (reg >> 16) | (mem & 0xffff0000)
	2   1abc   (reg >>  8) | (mem & 0xff000000)
	3   abcd   (reg      ) | (mem & 0x00000000)

	*/
}

void psxSWR(void)
{
	uint32_t shift = (_oB_ & 3) << 3;
	uint32_t mem   = iopMemRead32(_oB_ & 0xfffffffc);

	iopMemWrite32((_oB_ & 0xfffffffc), ( ( _u32(_rRt_) << shift ) |
			     (mem  & (0x00ffffff >> (24 - shift)) ) ) );
	/*
	Mem = 1234.  Reg = abcd

	0   abcd   (reg      ) | (mem & 0x00000000)
	1   bcd4   (reg <<  8) | (mem & 0x000000ff)
	2   cd34   (reg << 16) | (mem & 0x0000ffff)
	3   d234   (reg << 24) | (mem & 0x00ffffff)

	*/
}

/*********************************************************
* Moves between GPR and COPx                             *
* Format:  OP rt, fs                                     *
*********************************************************/
void psxMFC0(void)    { if (!_Rt_) return; _rRt_ = (int)_rFs_; }
void psxCFC0(void)    { if (!_Rt_) return; _rRt_ = (int)_rFs_; }

void psxMTC0(void)    { _rFs_ = _u32(_rRt_); }
void psxCTC0(void)    { _rFs_ = _u32(_rRt_); }

void psxCTC2(void)    { _c2dRd_ = _u32(_rRt_); };
/*********************************************************
* Unknown instruction (would generate an exception)       *
* Format:  ?                                             *
*********************************************************/
void psxNULL(void)    { }
void psxSPECIAL(void) { psxSPC[_Funct_](); }
void psxREGIMM(void)  { psxREG[_Rt_]();    }
void psxCOP0(void)    { psxCP0[_Rs_]();    }
void psxCOP2(void)    { psxCP2[_Funct_](); }
void psxBASIC(void)   { psxCP2BSC[_Rs_](); }

void(*psxBSC[64])(void) = {
	psxSPECIAL, psxREGIMM, psxJ   , psxJAL  , psxBEQ , psxBNE , psxBLEZ, psxBGTZ, //7
	psxADDI   , psxADDIU , psxSLTI, psxSLTIU, psxANDI, psxORI , psxXORI, psxLUI , //15
	psxCOP0   , psxNULL  , psxCOP2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //23
	psxNULL   , psxNULL  , psxNULL, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //31
	psxLB     , psxLH    , psxLWL , psxLW   , psxLBU , psxLHU , psxLWR , psxNULL, //39
	psxSB     , psxSH    , psxSWL , psxSW   , psxNULL, psxNULL, psxSWR , psxNULL, //47
	psxNULL   , psxNULL  , gteLWC2, psxNULL , psxNULL, psxNULL, psxNULL, psxNULL, //55
	psxNULL   , psxNULL  , gteSWC2, psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL //63
};


void(*psxSPC[64])(void) = {
	psxSLL , psxNULL , psxSRL , psxSRA , psxSLLV   , psxNULL , psxSRLV, psxSRAV,
	psxJR  , psxJALR , psxNULL, psxNULL, psxSYSCALL, psxBREAK, psxNULL, psxNULL,
	psxMFHI, psxMTHI , psxMFLO, psxMTLO, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxMULT, psxMULTU, psxDIV , psxDIVU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxADD , psxADDU , psxSUB , psxSUBU, psxAND    , psxOR   , psxXOR , psxNOR ,
	psxNULL, psxNULL , psxSLT , psxSLTU, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL,
	psxNULL, psxNULL , psxNULL, psxNULL, psxNULL   , psxNULL , psxNULL, psxNULL
};

void(*psxREG[32])(void) = {
	psxBLTZ  , psxBGEZ  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxBLTZAL, psxBGEZAL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL  , psxNULL  , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void(*psxCP0[32])(void) = {
	psxMFC0, psxNULL, psxCFC0, psxNULL, psxMTC0, psxNULL, psxCTC0, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxRFE , psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};

void (*psxCP2[64])(void) = {
	psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL, // 00
	psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL, // 08
	gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL, // 10
	psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL, // 18
	gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 20
	gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL, // 28
	gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 30
	psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT  // 38
};

void(*psxCP2BSC[32])(void) = {
	gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
	psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL
};
