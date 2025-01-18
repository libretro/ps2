#include "Common.h"
#include "VUops.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"

#include <cmath>
#include <float.h>

//Lower/Upper instructions can use that..
#define _Ft_ ((VU->code >> 16) & 0x1F)  // The rt part of the instruction register
#define _Fs_ ((VU->code >> 11) & 0x1F)  // The rd part of the instruction register
#define _Fd_ ((VU->code >>  6) & 0x1F)  // The sa part of the instruction register
#define _It_ (_Ft_ & 0xF)
#define _Is_ (_Fs_ & 0xF)
#define _Id_ (_Fd_ & 0xF)

#define _X ((VU->code >> 24) & 0x1)
#define _Y ((VU->code >> 23) & 0x1)
#define _Z ((VU->code >> 22) & 0x1)
#define _W ((VU->code >> 21) & 0x1)

#define _XYZW ((VU->code >> 21) & 0xF)

#define _Fsf_ ((VU->code >> 21) & 0x03)
#define _Ftf_ ((VU->code >> 23) & 0x03)

#define _Imm11_		(s32)(VU->code & 0x400 ? 0xfffffc00 | (VU->code & 0x3ff) : VU->code & 0x3ff)
#define _UImm11_	(s32)(VU->code & 0x7ff)

#define VI_BACKUP

alignas(16) static VECTOR RDzero;

/*****************************************/
/*          NEW FLAGS                    */ //By asadr. Thnkx F|RES :p
/*****************************************/

static __ri u32 VU_MAC_UPDATE( int shift, VURegs * VU, float f )
{
	u32 v = *(u32*)&f;
	int exp = (v >> 23) & 0xff;
	u32 s = v & 0x80000000;

	if (s)
		VU->macflag |= 0x0010<<shift;
	else
		VU->macflag &= ~(0x0010<<shift);

	if( f == 0 )
		VU->macflag = (VU->macflag & ~(0x1100<<shift)) | (0x0001<<shift);
	else
	{
		switch(exp)
		{
			case 0:
				VU->macflag = (VU->macflag&~(0x1000<<shift)) | (0x0101<<shift);
				return s;
			case 255:
				VU->macflag = (VU->macflag&~(0x0101<<shift)) | (0x1000<<shift);
				if (CHECK_VU_OVERFLOW((VU == &vuRegs[1]) ? 1 : 0))
					return s | 0x7f7fffff; /* max allowed */
				break;
			default:
				VU->macflag = (VU->macflag & ~(0x1101<<shift));
				break;
		}
	}
	return v;
}

#define VU_MACx_UPDATE(VU, x) VU_MAC_UPDATE(3, VU, x)
#define VU_MACy_UPDATE(VU, y) VU_MAC_UPDATE(2, VU, y)
#define VU_MACz_UPDATE(VU, z) VU_MAC_UPDATE(1, VU, z)
#define VU_MACw_UPDATE(VU, w) VU_MAC_UPDATE(0, VU, w)
#define VU_MACx_CLEAR(VU) ((VU)->macflag &= ~(0x1111<<3))
#define VU_MACy_CLEAR(VU) ((VU)->macflag &= ~(0x1111<<2))
#define VU_MACz_CLEAR(VU) ((VU)->macflag &= ~(0x1111<<1))
#define VU_MACw_CLEAR(VU) ((VU)->macflag &= ~(0x1111<<0))

#define VU_STAT_UPDATE(VU) \
	VU->statusflag = 0; \
	if (VU->macflag & 0x000F) VU->statusflag = 0x1; \
	if (VU->macflag & 0x00F0) VU->statusflag |= 0x2; \
	if (VU->macflag & 0x0F00) VU->statusflag |= 0x4; \
	if (VU->macflag & 0xF000) VU->statusflag |= 0x8

static __ri bool _vuFMACflush(VURegs* VU)
{
	bool didflush = false;

	for (int i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
			break;

		// Clip flags (Affected by CLIP instruction)
		if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

		// Normal FMAC instructions only affects Z/S/I/O, 
		// D/I are modified only by FDIV instructions
		// Sticky flags (Affected by FSSET)
		if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30) | (VU->fmac[i].statusflag & 0xFC0) | (VU->fmac[i].statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0) | (VU->fmac[i].statusflag & 0xF) | ((VU->fmac[i].statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

		VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;
		VU->fmaccount--;

		didflush = true;
	}

	return didflush;
}

static __ri bool _vuIALUflush(VURegs* VU)
{
	bool didflush = false;

	for (int i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
			break;

		VU->ialureadpos = (VU->ialureadpos + 1) & 3;
		VU->ialucount--;
		didflush = true;
	}
	return didflush;
}

static __ri bool _vuFDIVflush(VURegs* VU)
{
	if (VU->fdiv.enable != 0)
	{
		if ((VU->cycle - VU->fdiv.sCycle) >= VU->fdiv.Cycle)
		{
			VU->fdiv.enable = 0;
			VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
			// FDIV only affects D/I
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU->fdiv.statusflag & 0xC30);

			return true;
		}
	}
	return false;
}

static __ri bool _vuEFUflush(VURegs* VU)
{
	if (VU->efu.enable != 0)
	{
		if ((VU->cycle - VU->efu.sCycle) >= VU->efu.Cycle)
		{
			VU->efu.enable = 0;
			VU->VI[REG_P].UL = VU->efu.reg.UL;

			return true;
		}
	}

	return false;
}

// called at end of program
void _vuFlushAll(VURegs* VU)
{
	int i = 0;

	if (VU->fdiv.enable)
	{
		VU->fdiv.enable            = 0;
		VU->VI[REG_Q].UL           = VU->fdiv.reg.UL;
		VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU->fdiv.statusflag & 0xC30);

		if ((VU->cycle - VU->fdiv.sCycle) < VU->fdiv.Cycle)
			VU->cycle = VU->fdiv.sCycle + VU->fdiv.Cycle;
	}

	if (VU->efu.enable)
	{
		VU->efu.enable    = 0;
		VU->VI[REG_P].UL  = VU->efu.reg.UL;

		if ((VU->cycle - VU->efu.sCycle) < VU->efu.Cycle)
			VU->cycle = VU->efu.sCycle + VU->efu.Cycle;
	}

	for (i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
	{
		// Clip flags (Affected by CLIP instruction)
		if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

		// Normal FMAC instructions only affects Z/S/I/O, 
		// D/I are modified only by FDIV instructions
		// Sticky flags (Affected by FSSET)
		if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30) | (VU->fmac[i].statusflag & 0xFC0) | (VU->fmac[i].statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0) | (VU->fmac[i].statusflag & 0xF) | ((VU->fmac[i].statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

		VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;

		if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
			VU->cycle = VU->fmac[i].sCycle + VU->fmac[i].Cycle;

		VU->fmaccount--;
	}

	for (i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
	{
		VU->ialureadpos = (VU->ialureadpos + 1) & 3;

		if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
			VU->cycle = VU->ialu[i].sCycle + VU->ialu[i].Cycle;

		VU->ialucount--;
	}
}

__fi void _vuTestPipes(VURegs* VU)
{
	bool flushed;

	do
	{
		flushed = false;
		flushed |= _vuFMACflush(VU);
		flushed |= _vuFDIVflush(VU);
		flushed |= _vuEFUflush(VU);
		flushed |= _vuIALUflush(VU);
	} while (flushed);

	if (VU == &vuRegs[1])
	{
		if (vuRegs[1].xgkickenable)
			_vuXGKICKTransfer((vuRegs[1].cycle - vuRegs[1].xgkicklastcycle) - 1, false);
	}
}

static void _vuFMACTestStall(VURegs* VU, u32 reg, u32 xyzw)
{
	u32 i = 0;

	for (int currentpipe = VU->fmacreadpos; i < VU->fmaccount; currentpipe = (currentpipe + 1) & 3, i++)
	{
		//Check if enough cycles have passed for this FMAC position
		if ((VU->cycle - VU->fmac[currentpipe].sCycle) >= VU->fmac[currentpipe].Cycle)
			continue;

		// Check if the regs match
		if ((VU->fmac[currentpipe].regupper == reg && VU->fmac[currentpipe].xyzwupper & xyzw)
			|| (VU->fmac[currentpipe].reglower == reg && VU->fmac[currentpipe].xyzwlower & xyzw))
		{
			u32 newCycle = VU->fmac[currentpipe].Cycle + VU->fmac[currentpipe].sCycle;

			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
}

static __fi void _vuTestFMACStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VFread0)
	{
		_vuFMACTestStall(VU, VUregsn->VFread0, VUregsn->VFr0xyzw);
	}
	if (VUregsn->VFread1)
	{
		_vuFMACTestStall(VU, VUregsn->VFread1, VUregsn->VFr1xyzw);
	}
}

static __fi void _vuTestFDIVStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	_vuTestFMACStalls(VU, VUregsn);

	if (VU->fdiv.enable != 0)
	{
		u32 newCycle = VU->fdiv.Cycle + VU->fdiv.sCycle;
		if (newCycle > VU->cycle)
			VU->cycle = newCycle;
	}
}

static __fi void _vuTestEFUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	_vuTestFMACStalls(VU, VUregsn);

	if (VU->efu.enable == 0)
		return;

	// With EFU commands they have a throughput/latency that doesn't match, this means if a stall occurs
	// The stall is released 1 cycle before P is updated. However there is no other command that can read
	// P on the same cycle as the stall is released, and if the stall is caused by an EFU command other
	// than WAITP, we're going to overwrite the value in the pipeline, which will break everything.
	// So the TL;DR of this is that we should be safe to release 1 cycle early and write back P
	VU->efu.Cycle -= 1;
	u32 newCycle = VU->efu.sCycle + VU->efu.Cycle;

	if (newCycle > VU->cycle)
		VU->cycle = newCycle;
}

static __fi void _vuTestALUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	u32 i = 0;

	for (int currentpipe = VU->ialureadpos; i < VU->ialucount; currentpipe = (currentpipe + 1) & 3, i++)
	{
		if ((VU->cycle - VU->ialu[currentpipe].sCycle) >= VU->ialu[currentpipe].Cycle)
			continue;

		if (VU->ialu[currentpipe].reg & VUregsn->VIread) // Read and written VI regs share the same register
		{
			u32 newCycle = VU->ialu[currentpipe].Cycle + VU->ialu[currentpipe].sCycle;

			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
}

__fi void _vuTestUpperStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->pipe == VUPIPE_FMAC)
		_vuTestFMACStalls(VU, VUregsn);
}


__fi void _vuTestLowerStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe)
	{
		case VUPIPE_FMAC: _vuTestFMACStalls(VU, VUregsn); break;
		case VUPIPE_FDIV: _vuTestFDIVStalls(VU, VUregsn); break;
		case VUPIPE_EFU:  _vuTestEFUStalls(VU, VUregsn); break;
		case VUPIPE_BRANCH: _vuTestALUStalls(VU, VUregsn); break;
	}
}

__fi void _vuClearFMAC(VURegs* VU)
{
	int i = VU->fmacwritepos;

	memset(&VU->fmac[i], 0, sizeof(fmacPipe));
	VU->fmaccount++;
}

static __ri void _vuAddFMACStalls(VURegs* VU, _VURegsNum* VUregsn, bool isUpper)
{
	int i = VU->fmacwritepos;

	VU->fmac[i].sCycle = VU->cycle;
	VU->fmac[i].Cycle = 4;

	if (isUpper)
	{
		VU->fmac[i].regupper = VUregsn->VFwrite;
		VU->fmac[i].xyzwupper = VUregsn->VFwxyzw;
		VU->fmac[i].flagreg = VUregsn->VIwrite;
	}
	else
	{
		VU->fmac[i].reglower = VUregsn->VFwrite;
		VU->fmac[i].xyzwlower = VUregsn->VFwxyzw;
		VU->fmac[i].flagreg |= VUregsn->VIwrite;
	}

	VU->fmac[i].macflag = VU->macflag;
	VU->fmac[i].statusflag = VU->statusflag;
	VU->fmac[i].clipflag = VU->clipflag;
}


static __ri void _vuFDIVAdd(VURegs* VU, int cycles)
{
	VU->fdiv.enable = 1;
	VU->fdiv.sCycle = VU->cycle;
	VU->fdiv.Cycle = cycles;
	VU->fdiv.reg.F = VU->q.F;
	VU->fdiv.statusflag = VU->statusflag;
}

static __ri void _vuEFUAdd(VURegs* VU, int cycles)
{
	VU->efu.enable = 1;
	VU->efu.sCycle = VU->cycle;
	VU->efu.Cycle = cycles;
	VU->efu.reg.F = VU->p.F;
}

static __ri void _vuAddIALUStalls(VURegs* VU, _VURegsNum* VUregsn)
{

	if (VUregsn->cycles == 0)
		return;

	int i = VU->ialuwritepos;

	VU->ialu[i].sCycle = VU->cycle;
	VU->ialu[i].Cycle = VUregsn->cycles;
	VU->ialu[i].reg = VUregsn->VIwrite;

	VU->ialuwritepos = (VU->ialuwritepos + 1) & 3;
	VU->ialucount++;
}

static __fi void _vuAddFDIVStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VIwrite & (1 << REG_Q))
		_vuFDIVAdd(VU, VUregsn->cycles);
}

static __fi void _vuAddEFUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VIwrite & (1 << REG_P))
		_vuEFUAdd(VU, VUregsn->cycles);
}

__fi void _vuAddUpperStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->pipe == VUPIPE_FMAC)
		_vuAddFMACStalls(VU, VUregsn, true);
}

__fi void _vuAddLowerStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe)
	{
		case VUPIPE_FMAC: _vuAddFMACStalls(VU, VUregsn, false); break;
		case VUPIPE_FDIV: _vuAddFDIVStalls(VU, VUregsn); break;
		case VUPIPE_EFU:  _vuAddEFUStalls(VU, VUregsn); break;
		case VUPIPE_IALU: _vuAddIALUStalls(VU, VUregsn); break;
	}
}

__fi void _vuBackupVI(VURegs* VU, u32 reg)
{
#ifdef VI_BACKUP
	if (VU->VIBackupCycles && reg == VU->VIRegNumber)
	{
		//On repeat writes we need to remember the value from before the chain
		VU->VIBackupCycles = 2;
		return;
	}

	VU->VIBackupCycles = 2;
	VU->VIRegNumber = reg;
	VU->VIOldValue = VU->VI[reg].US[0];
#endif
}

//interpreter hacks, WIP
//#define INT_VUDOUBLEHACK

/******************************/
/*   VU Upper instructions    */
/******************************/
static float vuDouble(u32 f)
{
#ifndef INT_VUDOUBLEHACK
	switch (f & 0x7f800000)
	{
		case 0x0:
			f &= 0x80000000;
			break;
		case 0x7f800000:
			if (EmuConfig.Cpu.Recompiler.vu0ExtraOverflow)
			{
				u32 d = (f & 0x80000000) | 0x7f7fffff;
				return *(float*)&d;
			}
			break;
	}
#endif
	return *(float*)&f;
}

static __fi float vuADD_TriAceHack(u32 a, u32 b)
{
	// On VU0 TriAce Games use ADDi and expects these bit-perfect results:
	//if (a == 0xb3e2a619 && b == 0x42546666) return vuDouble(0x42546666);
	//if (a == 0x8b5b19e9 && b == 0xc7f079b3) return vuDouble(0xc7f079b3);
	//if (a == 0x4b1ed4a8 && b == 0x43a02666) return vuDouble(0x4b1ed5e7);
	//if (a == 0x7d1ca47b && b == 0x42f23333) return vuDouble(0x7d1ca47b);

	// In the 3rd case, some other rounding error is giving us incorrect
	// operands ('a' is wrong); and therefor an incorrect result.
	// We're getting:        0x4b1ed4a8 + 0x43a02666 = 0x4b1ed5e8
	// We should be getting: 0x4b1ed4a7 + 0x43a02666 = 0x4b1ed5e7
	// microVU gets the correct operands and result. The interps likely
	// don't get it due to rounding towards nearest in other calculations.

	// microVU uses something like this to get TriAce games working,
	// but VU interpreters don't seem to need it currently:

	// Update Sept 2021, now the interpreters don't suck, they do - Refraction
	s32 aExp = (a >> 23) & 0xff;
	s32 bExp = (b >> 23) & 0xff;
	if (aExp - bExp >= 25) b &= 0x80000000;
	if (aExp - bExp <=-25) a &= 0x80000000;
	return vuDouble(a) + vuDouble(b);
}

void _vuABS(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X){ VU->VF[_Ft_].f.x = fabs(vuDouble(VU->VF[_Fs_].i.x)); }
	if (_Y){ VU->VF[_Ft_].f.y = fabs(vuDouble(VU->VF[_Fs_].i.y)); }
	if (_Z){ VU->VF[_Ft_].f.z = fabs(vuDouble(VU->VF[_Fs_].i.z)); }
	if (_W){ VU->VF[_Ft_].f.w = fabs(vuDouble(VU->VF[_Fs_].i.w)); }
}


static __fi void _vuADD(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuADDi(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (CHECK_VUADDSUBHACK)
	{
		if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuADD_TriAceHack(VU->VF[_Fs_].i.x, VU->VI[REG_I].UL));} else VU_MACx_CLEAR(VU);
		if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuADD_TriAceHack(VU->VF[_Fs_].i.y, VU->VI[REG_I].UL));} else VU_MACy_CLEAR(VU);
		if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuADD_TriAceHack(VU->VF[_Fs_].i.z, VU->VI[REG_I].UL));} else VU_MACz_CLEAR(VU);
		if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuADD_TriAceHack(VU->VF[_Fs_].i.w, VU->VI[REG_I].UL));} else VU_MACw_CLEAR(VU);
	}
	else
	{
		if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VI[REG_I].UL));} else VU_MACx_CLEAR(VU);
		if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VI[REG_I].UL));} else VU_MACy_CLEAR(VU);
		if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VI[REG_I].UL));} else VU_MACz_CLEAR(VU);
		if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + vuDouble(VU->VI[REG_I].UL));} else VU_MACw_CLEAR(VU);
	}
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDq(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VI[REG_Q].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VI[REG_Q].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VI[REG_Q].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + vuDouble(VU->VI[REG_Q].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuADDx(VURegs* VU)
{
	float ftx;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx=vuDouble(VU->VF[_Ft_].i.x);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + ftx); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + ftx); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + ftx); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + ftx); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDy(VURegs* VU)
{
	float fty;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	fty=vuDouble(VU->VF[_Ft_].i.y);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + fty);} else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + fty);} else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + fty);} else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + fty);} else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDz(VURegs* VU)
{
	float ftz;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftz=vuDouble(VU->VF[_Ft_].i.z);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + ftz); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + ftz); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + ftz); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + ftz); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDw(VURegs* VU)
{
	float ftw;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftw=vuDouble(VU->VF[_Ft_].i.w);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + ftw); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + ftw); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + ftw); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + ftw); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDA(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAi(VURegs* VU)
{
	float ti = vuDouble(VU->VI[REG_I].UL);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + ti); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + ti); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + ti); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + ti); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAq(VURegs* VU)
{
	float tf = vuDouble(VU->VI[REG_Q].UL);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + tf); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + tf); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + tf); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + tf); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAx(VURegs* VU)
{
	float tx = vuDouble(VU->VF[_Ft_].i.x);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + tx); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + tx); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + tx); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + tx); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAy(VURegs* VU)
{
	float ty = vuDouble(VU->VF[_Ft_].i.y);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + ty); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + ty); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + ty); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + ty); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAz(VURegs* VU)
{
	float tz = vuDouble(VU->VF[_Ft_].i.z);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + tz); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + tz); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + tz); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + tz); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuADDAw(VURegs* VU)
{
	float tw = vuDouble(VU->VF[_Ft_].i.w);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) + tw); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) + tw); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) + tw); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) + tw); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuSUB(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VF[_Ft_].i.x));  } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VF[_Ft_].i.y));  } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VF[_Ft_].i.z));  } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VF[_Ft_].i.w));  } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBi(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBq(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBx(VURegs* VU)
{
	float ftx;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx=vuDouble(VU->VF[_Ft_].i.x);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - ftx); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - ftx); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - ftx); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - ftx); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBy(VURegs* VU)
{
	float fty;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	fty=vuDouble(VU->VF[_Ft_].i.y);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - fty); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - fty); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - fty); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - fty); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBz(VURegs* VU)
{
	float ftz;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftz=vuDouble(VU->VF[_Ft_].i.z);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - ftz); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - ftz); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - ftz); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - ftz); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBw(VURegs* VU)
{
	float ftw;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftw=vuDouble(VU->VF[_Ft_].i.w);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - ftw); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - ftw); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - ftw); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - ftw); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuSUBA(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAi(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VI[REG_I].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAq(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - vuDouble(VU->VI[REG_Q].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAx(VURegs* VU)
{
	float tx = vuDouble(VU->VF[_Ft_].i.x);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - tx); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - tx); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - tx); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - tx); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAy(VURegs* VU)
{
	float ty = vuDouble(VU->VF[_Ft_].i.y);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - ty); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - ty); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - ty); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - ty); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAz(VURegs* VU)
{
	float tz = vuDouble(VU->VF[_Ft_].i.z);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - tz); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - tz); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - tz); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - tz); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuSUBAw(VURegs* VU)
{
	float tw = vuDouble(VU->VF[_Ft_].i.w);

	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) - tw); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) - tw); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) - tw); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) - tw); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMUL(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULi(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULq(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULx(VURegs* VU)
{
	float ftx;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

 	ftx=vuDouble(VU->VF[_Ft_].i.x);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * ftx); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * ftx); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * ftx); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * ftx); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuMULy(VURegs* VU)
{
	float fty;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

 	fty=vuDouble(VU->VF[_Ft_].i.y);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * fty); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * fty); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * fty); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * fty); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULz(VURegs* VU)
{
	float ftz;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

 	ftz=vuDouble(VU->VF[_Ft_].i.z);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * ftz); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * ftz); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * ftz); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * ftz); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULw(VURegs* VU)
{
	float ftw;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftw=vuDouble(VU->VF[_Ft_].i.w);
	if (_X){ dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * ftw); } else VU_MACx_CLEAR(VU);
	if (_Y){ dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * ftw); } else VU_MACy_CLEAR(VU);
	if (_Z){ dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * ftw); } else VU_MACz_CLEAR(VU);
	if (_W){ dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * ftw); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuMULA(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAi(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_I].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAq(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_Q].UL)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAx(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.x)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAy(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.y)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAz(VURegs* VU)
{
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.z)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMULAw(VURegs*  VU) {
	if (_X){ VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACx_CLEAR(VU);
	if (_Y){ VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACy_CLEAR(VU);
	if (_Z){ VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACz_CLEAR(VU);
	if (_W){ VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADD(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}


static __fi void _vuMADDi(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_I].UL))); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_I].UL))); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_I].UL))); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_I].UL))); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDq(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDx(VURegs* VU)
{
	float ftx;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx=vuDouble(VU->VF[_Ft_].i.x);
	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * ftx)); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * ftx)); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * ftx)); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * ftx)); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDy(VURegs* VU)
{
	float fty;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	fty=vuDouble(VU->VF[_Ft_].i.y);
	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * fty)); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * fty)); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * fty)); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * fty)); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDz(VURegs* VU)
{
	float ftz;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftz=vuDouble(VU->VF[_Ft_].i.z);
	if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * ftz)); else VU_MACx_CLEAR(VU);
	if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * ftz)); else VU_MACy_CLEAR(VU);
	if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * ftz)); else VU_MACz_CLEAR(VU);
	if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * ftz)); else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDw(VURegs* VU)
{
	float ftw;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftw=vuDouble(VU->VF[_Ft_].i.w);
    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * ftw)); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * ftw)); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * ftw)); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * ftw)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDA(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + (vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + (vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + (vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + (vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAi(VURegs* VU)
{
	float ti = vuDouble(VU->VI[REG_I].UL);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * ti)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * ti)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * ti)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * ti)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAq(VURegs* VU)
{
	float tq = vuDouble(VU->VI[REG_Q].UL);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * tq)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * tq)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * tq)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * tq)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAx(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAy(VURegs*  VU) {
	if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAz(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMADDAw(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) + ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) + ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) + ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) + ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUB(VURegs* VU)
{
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBi(VURegs* VU)
{
	float ti = vuDouble(VU->VI[REG_I].UL);
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * ti  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * ti  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * ti  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * ti  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBq(VURegs* VU)
{
	float tq = vuDouble(VU->VI[REG_Q].UL);
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x)  - ( vuDouble(VU->VF[_Fs_].i.x) * tq  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y)  - ( vuDouble(VU->VF[_Fs_].i.y) * tq  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z)  - ( vuDouble(VU->VF[_Fs_].i.z) * tq  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w)  - ( vuDouble(VU->VF[_Fs_].i.w) * tq  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}


static __fi void _vuMSUBx(VURegs* VU)
{
	float ftx;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx=vuDouble(VU->VF[_Ft_].i.x);
    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x)  - ( vuDouble(VU->VF[_Fs_].i.x) * ftx  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y)  - ( vuDouble(VU->VF[_Fs_].i.y) * ftx  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z)  - ( vuDouble(VU->VF[_Fs_].i.z) * ftx  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w)  - ( vuDouble(VU->VF[_Fs_].i.w) * ftx  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}


static __fi void _vuMSUBy(VURegs* VU)
{
	float fty;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	fty=vuDouble(VU->VF[_Ft_].i.y);
    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x)  - ( vuDouble(VU->VF[_Fs_].i.x) * fty  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y)  - ( vuDouble(VU->VF[_Fs_].i.y) * fty  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z)  - ( vuDouble(VU->VF[_Fs_].i.z) * fty  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w)  - ( vuDouble(VU->VF[_Fs_].i.w) * fty  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}


static __fi void _vuMSUBz(VURegs* VU)
{
	float ftz;
	VECTOR* dst;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftz=vuDouble(VU->VF[_Ft_].i.z);
    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x)  - ( vuDouble(VU->VF[_Fs_].i.x) * ftz  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y)  - ( vuDouble(VU->VF[_Fs_].i.y) * ftz  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z)  - ( vuDouble(VU->VF[_Fs_].i.z) * ftz  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w)  - ( vuDouble(VU->VF[_Fs_].i.w) * ftz  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBw(VURegs* VU)
{
	float ftw;
	VECTOR * dst;
    if (_Fd_ == 0) dst = &RDzero;
	else dst = &VU->VF[_Fd_];

	ftw=vuDouble(VU->VF[_Ft_].i.w);
    if (_X) dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x)  - ( vuDouble(VU->VF[_Fs_].i.x) * ftw  ) ); else VU_MACx_CLEAR(VU);
    if (_Y) dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y)  - ( vuDouble(VU->VF[_Fs_].i.y) * ftw  ) ); else VU_MACy_CLEAR(VU);
    if (_Z) dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z)  - ( vuDouble(VU->VF[_Fs_].i.z) * ftw  ) ); else VU_MACz_CLEAR(VU);
    if (_W) dst->i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w)  - ( vuDouble(VU->VF[_Fs_].i.w) * ftw  ) ); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}


static __fi void _vuMSUBA(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.x))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.y))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.z))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VF[_Ft_].i.w))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAi(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_I].UL))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_I].UL))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_I].UL))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_I].UL))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAq(VURegs*  VU) {
    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * vuDouble(VU->VI[REG_Q].UL))); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAx(VURegs* VU)
{
	float tx = vuDouble(VU->VF[_Ft_].i.x);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * tx)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * tx)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * tx)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * tx)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAy(VURegs* VU)
{
	float ty = vuDouble(VU->VF[_Ft_].i.y);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * ty)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * ty)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * ty)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * ty)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAz(VURegs* VU)
{
	float tz = vuDouble(VU->VF[_Ft_].i.z);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * tz)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * tz)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * tz)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * tz)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

static __fi void _vuMSUBAw(VURegs* VU)
{
	float tw = vuDouble(VU->VF[_Ft_].i.w);

    if (_X) VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - ( vuDouble(VU->VF[_Fs_].i.x) * tw)); else VU_MACx_CLEAR(VU);
    if (_Y) VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - ( vuDouble(VU->VF[_Fs_].i.y) * tw)); else VU_MACy_CLEAR(VU);
    if (_Z) VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - ( vuDouble(VU->VF[_Fs_].i.z) * tw)); else VU_MACz_CLEAR(VU);
    if (_W) VU->ACC.i.w = VU_MACw_UPDATE(VU, vuDouble(VU->ACC.i.w) - ( vuDouble(VU->VF[_Fs_].i.w) * tw)); else VU_MACw_CLEAR(VU);
    VU_STAT_UPDATE(VU);
}

// The functions below are floating point semantics min/max on integer representations to get
// the effect of a floating point min/max without issues with denormal and special numbers.

static __fi u32 fp_max(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::min<s32>(a, b) : std::max<s32>(a, b);
}

static __fi u32 fp_min(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::max<s32>(a, b) : std::min<s32>(a, b);
}

static __fi void _vuMAX(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, VU->VF[_Ft_].i.x);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, VU->VF[_Ft_].i.y);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, VU->VF[_Ft_].i.z);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, VU->VF[_Ft_].i.w);
}

static __fi void _vuMAXi(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, VU->VI[REG_I].UL);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, VU->VI[REG_I].UL);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, VU->VI[REG_I].UL);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, VU->VI[REG_I].UL);
}

static __fi void _vuMAXx(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftx = VU->VF[_Ft_].i.x;
	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, ftx);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, ftx);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, ftx);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, ftx);
}

static __fi void _vuMAXy(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 fty = VU->VF[_Ft_].i.y;
	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, fty);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, fty);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, fty);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, fty);
}

static __fi void _vuMAXz(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftz = VU->VF[_Ft_].i.z;
	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, ftz);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, ftz);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, ftz);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, ftz);
}

static __fi void _vuMAXw(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftw = VU->VF[_Ft_].i.w;
	if (_X) VU->VF[_Fd_].i.x = fp_max(VU->VF[_Fs_].i.x, ftw);
	if (_Y) VU->VF[_Fd_].i.y = fp_max(VU->VF[_Fs_].i.y, ftw);
	if (_Z) VU->VF[_Fd_].i.z = fp_max(VU->VF[_Fs_].i.z, ftw);
	if (_W) VU->VF[_Fd_].i.w = fp_max(VU->VF[_Fs_].i.w, ftw);
}

static __fi void _vuMINI(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	/* ft is bc */
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, VU->VF[_Ft_].i.x);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, VU->VF[_Ft_].i.y);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, VU->VF[_Ft_].i.z);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, VU->VF[_Ft_].i.w);
}

static __fi void _vuMINIi(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	/* ft is bc */
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, VU->VI[REG_I].UL);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, VU->VI[REG_I].UL);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, VU->VI[REG_I].UL);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, VU->VI[REG_I].UL);
}

static __fi void _vuMINIx(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftx = VU->VF[_Ft_].i.x;
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, ftx);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, ftx);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, ftx);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, ftx);
}

static __fi void _vuMINIy(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 fty = VU->VF[_Ft_].i.y;
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, fty);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, fty);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, fty);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, fty);
}

static __fi void _vuMINIz(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftz = VU->VF[_Ft_].i.z;
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, ftz);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, ftz);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, ftz);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, ftz);
}

static __fi void _vuMINIw(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	u32 ftw = VU->VF[_Ft_].i.w;
	if (_X) VU->VF[_Fd_].i.x = fp_min(VU->VF[_Fs_].i.x, ftw);
	if (_Y) VU->VF[_Fd_].i.y = fp_min(VU->VF[_Fs_].i.y, ftw);
	if (_Z) VU->VF[_Fd_].i.z = fp_min(VU->VF[_Fs_].i.z, ftw);
	if (_W) VU->VF[_Fd_].i.w = fp_min(VU->VF[_Fs_].i.w, ftw);
}

static __fi void _vuOPMULA(VURegs* VU)
{
	VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.z));
	VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.x));
	VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.y));
	VU_STAT_UPDATE(VU);
}

static __fi void _vuOPMSUB(VURegs* VU)
{
	VECTOR* dst;
	float ftx, fty, ftz;
	float fsx, fsy, fsz;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx = vuDouble(VU->VF[_Ft_].i.x);
	fty = vuDouble(VU->VF[_Ft_].i.y);
	ftz = vuDouble(VU->VF[_Ft_].i.z);
	fsx = vuDouble(VU->VF[_Fs_].i.x);
	fsy = vuDouble(VU->VF[_Fs_].i.y);
	fsz = vuDouble(VU->VF[_Fs_].i.z);

	dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - fsy * ftz);
	dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - fsz * ftx);
	dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - fsx * fty);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuNOP(VURegs* VU)
{
}

static __fi s32 float_to_int(float value)
{
	if (value >= 2147483647.0)
		return 2147483647LL;
	if (value <= -2147483648.0)
		return -2147483648LL;
	return value;
}

static __fi void _vuFTOI0(VURegs*  VU) {
	if (_Ft_ == 0) return;

	if (_X) VU->VF[_Ft_].SL[0] = float_to_int(vuDouble(VU->VF[_Fs_].i.x));
	if (_Y) VU->VF[_Ft_].SL[1] = float_to_int(vuDouble(VU->VF[_Fs_].i.y));
	if (_Z) VU->VF[_Ft_].SL[2] = float_to_int(vuDouble(VU->VF[_Fs_].i.z));
	if (_W) VU->VF[_Ft_].SL[3] = float_to_int(vuDouble(VU->VF[_Fs_].i.w));
}

static __fi void _vuFTOI4(VURegs*  VU) {
	if (_Ft_ == 0) return;

	if (_X) VU->VF[_Ft_].SL[0] = float_to_int(float_to_int4(vuDouble(VU->VF[_Fs_].i.x)));
	if (_Y) VU->VF[_Ft_].SL[1] = float_to_int(float_to_int4(vuDouble(VU->VF[_Fs_].i.y)));
	if (_Z) VU->VF[_Ft_].SL[2] = float_to_int(float_to_int4(vuDouble(VU->VF[_Fs_].i.z)));
	if (_W) VU->VF[_Ft_].SL[3] = float_to_int(float_to_int4(vuDouble(VU->VF[_Fs_].i.w)));
}

static __fi void _vuFTOI12(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].SL[0] = float_to_int(float_to_int12(vuDouble(VU->VF[_Fs_].i.x)));
	if (_Y) VU->VF[_Ft_].SL[1] = float_to_int(float_to_int12(vuDouble(VU->VF[_Fs_].i.y)));
	if (_Z) VU->VF[_Ft_].SL[2] = float_to_int(float_to_int12(vuDouble(VU->VF[_Fs_].i.z)));
	if (_W) VU->VF[_Ft_].SL[3] = float_to_int(float_to_int12(vuDouble(VU->VF[_Fs_].i.w)));
}

static __fi void _vuFTOI15(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].SL[0] = float_to_int(float_to_int15(vuDouble(VU->VF[_Fs_].i.x)));
	if (_Y) VU->VF[_Ft_].SL[1] = float_to_int(float_to_int15(vuDouble(VU->VF[_Fs_].i.y)));
	if (_Z) VU->VF[_Ft_].SL[2] = float_to_int(float_to_int15(vuDouble(VU->VF[_Fs_].i.z)));
	if (_W) VU->VF[_Ft_].SL[3] = float_to_int(float_to_int15(vuDouble(VU->VF[_Fs_].i.w)));
}

static __fi void _vuITOF0(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].f.x = (float)VU->VF[_Fs_].SL[0];
	if (_Y) VU->VF[_Ft_].f.y = (float)VU->VF[_Fs_].SL[1];
	if (_Z) VU->VF[_Ft_].f.z = (float)VU->VF[_Fs_].SL[2];
	if (_W) VU->VF[_Ft_].f.w = (float)VU->VF[_Fs_].SL[3];
}

static __fi void _vuITOF4(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].f.x = int4_to_float(VU->VF[_Fs_].SL[0]);
	if (_Y) VU->VF[_Ft_].f.y = int4_to_float(VU->VF[_Fs_].SL[1]);
	if (_Z) VU->VF[_Ft_].f.z = int4_to_float(VU->VF[_Fs_].SL[2]);
	if (_W) VU->VF[_Ft_].f.w = int4_to_float(VU->VF[_Fs_].SL[3]);
}

static __fi void _vuITOF12(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].f.x = int12_to_float(VU->VF[_Fs_].SL[0]);
	if (_Y) VU->VF[_Ft_].f.y = int12_to_float(VU->VF[_Fs_].SL[1]);
	if (_Z) VU->VF[_Ft_].f.z = int12_to_float(VU->VF[_Fs_].SL[2]);
	if (_W) VU->VF[_Ft_].f.w = int12_to_float(VU->VF[_Fs_].SL[3]);
}

static __fi void _vuITOF15(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].f.x = int15_to_float(VU->VF[_Fs_].SL[0]);
	if (_Y) VU->VF[_Ft_].f.y = int15_to_float(VU->VF[_Fs_].SL[1]);
	if (_Z) VU->VF[_Ft_].f.z = int15_to_float(VU->VF[_Fs_].SL[2]);
	if (_W) VU->VF[_Ft_].f.w = int15_to_float(VU->VF[_Fs_].SL[3]);
}

static __fi void _vuCLIP(VURegs* VU)
{
	float value = fabs(vuDouble(VU->VF[_Ft_].i.w));

	VU->clipflag <<= 6;
	if ( vuDouble(VU->VF[_Fs_].i.x) > +value ) VU->clipflag|= 0x01;
	if ( vuDouble(VU->VF[_Fs_].i.x) < -value ) VU->clipflag|= 0x02;
	if ( vuDouble(VU->VF[_Fs_].i.y) > +value ) VU->clipflag|= 0x04;
	if ( vuDouble(VU->VF[_Fs_].i.y) < -value ) VU->clipflag|= 0x08;
	if ( vuDouble(VU->VF[_Fs_].i.z) > +value ) VU->clipflag|= 0x10;
	if ( vuDouble(VU->VF[_Fs_].i.z) < -value ) VU->clipflag|= 0x20;
	VU->clipflag = VU->clipflag & 0xFFFFFF;
}

/******************************/
/*   VU Lower instructions    */
/******************************/

static __fi void _vuDIV(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);
	float fs = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	VU->statusflag &= ~0x30;

	if (ft == 0.0)
	{
		if (fs == 0.0)
			VU->statusflag |= 0x10;
		else
			VU->statusflag |= 0x20;

		if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
			(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
			VU->q.UL = 0xFF7FFFFF;
		else
			VU->q.UL = 0x7F7FFFFF;
	}
	else
	{
		VU->q.F = fs / ft;
		VU->q.F = vuDouble(VU->q.UL);
	}
}

static __fi void _vuSQRT(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);

	VU->statusflag &= ~0x30;

	if (ft < 0.0)
		VU->statusflag |= 0x10;
	VU->q.F = sqrt(fabs(ft));
	VU->q.F = vuDouble(VU->q.UL);
}

static __fi void _vuRSQRT(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);
	float fs = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);
	float temp;

	VU->statusflag &= ~0x30;

	if (ft == 0.0)
	{
		VU->statusflag |= 0x20;

		if (fs != 0)
		{
			if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
				(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
				VU->q.UL = 0xFF7FFFFF;
			else
				VU->q.UL = 0x7F7FFFFF;
		}
		else
		{
			if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
				(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
				VU->q.UL = 0x80000000;
			else
				VU->q.UL = 0;

			VU->statusflag |= 0x10;
		}
	}
	else
	{
		if (ft < 0.0)
			VU->statusflag |= 0x10;

		temp    = sqrt(fabs(ft));
		VU->q.F = fs / temp;
		VU->q.F = vuDouble(VU->q.UL);
	}
}

static __fi void _vuIADDI(VURegs* VU)
{
	s16 imm = ((VU->code >> 6) & 0x1f);
	imm     = ((imm & 0x10 ? 0xfff0 : 0) | (imm & 0xf));
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] + imm;
}

static __fi void _vuIADDIU(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] + (((VU->code >> 10) & 0x7800) | (VU->code & 0x7ff));
}

static __fi void _vuIADD(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].SS[0] = VU->VI[_Is_].SS[0] + VU->VI[_It_].SS[0];
}

static __fi void _vuIAND(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].US[0] = VU->VI[_Is_].US[0] & VU->VI[_It_].US[0];
}

static __fi void _vuIOR(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].US[0] = VU->VI[_Is_].US[0] | VU->VI[_It_].US[0];
}

static __fi void _vuISUB(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].SS[0] = VU->VI[_Is_].SS[0] - VU->VI[_It_].SS[0];
}

static __fi void _vuISUBIU(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] - (((VU->code >> 10) & 0x7800) | (VU->code & 0x7ff));
}

static __fi void _vuMOVE(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].UL[0] = VU->VF[_Fs_].UL[0];
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VF[_Fs_].UL[1];
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VF[_Fs_].UL[2];
	if (_W) VU->VF[_Ft_].UL[3] = VU->VF[_Fs_].UL[3];
}

static __fi void _vuMFIR(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].SL[0] = (s32)VU->VI[_Is_].SS[0];
	if (_Y) VU->VF[_Ft_].SL[1] = (s32)VU->VI[_Is_].SS[0];
	if (_Z) VU->VF[_Ft_].SL[2] = (s32)VU->VI[_Is_].SS[0];
	if (_W) VU->VF[_Ft_].SL[3] = (s32)VU->VI[_Is_].SS[0];
}

static __fi void _vuMTIR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].US[0] = *(u16*)&VU->VF[_Fs_].F[_Fsf_];
}

static __fi void _vuMR32(VURegs* VU)
{
	u32 tx;
	if (_Ft_ == 0)
		return;

	tx = VU->VF[_Fs_].i.x;
	if (_X) VU->VF[_Ft_].i.x = VU->VF[_Fs_].i.y;
	if (_Y) VU->VF[_Ft_].i.y = VU->VF[_Fs_].i.z;
	if (_Z) VU->VF[_Ft_].i.z = VU->VF[_Fs_].i.w;
	if (_W) VU->VF[_Ft_].i.w = tx;
}

// --------------------------------------------------------------------------------------
//  Load / Store Instructions (VU Interpreter)
// --------------------------------------------------------------------------------------

__fi u32* GET_VU_MEM(VURegs* VU, u32 addr) // non-static, also used by sVU for now.
{
	if (VU == &vuRegs[1])
		return (u32*)(vuRegs[1].Mem + (addr & 0x3fff));
	else if (addr & 0x4000)
		return (u32*)((u8*)vuRegs[1].VF + (addr & 0x3ff)); // get VF and VI regs (they're mapped to 0x4xx0 in VU0 mem!)
	return (u32*)(vuRegs[0].Mem + (addr & 0xfff)); // for addr 0x0000 to 0x4000 just wrap around
}

static __ri void _vuLQ(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	s16 imm  = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);
 	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
	if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
	if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
	if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
}

static __ri void _vuLQD(VURegs* VU)
{
	_vuBackupVI(VU, _Is_);
	if (_Is_ != 0)
		VU->VI[_Is_].US[0]--;
	if (_Ft_ == 0)
		return;

	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
	if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
	if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
	if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
}

static __ri void _vuLQI(VURegs* VU)
{
	_vuBackupVI(VU, _Is_);
	if (_Ft_)
	{
		u32 addr = (VU->VI[_Is_].US[0] * 16);
		u32* ptr = (u32*)GET_VU_MEM(VU, addr);
		if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
		if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
		if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
		if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
	}
	if (_Fs_ != 0)
		VU->VI[_Is_].US[0]++;
}

static __ri void _vuSQ(VURegs* VU)
{
	s16 imm  = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_It_].SS[0]) * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
}

static __ri void _vuSQD(VURegs* VU)
{
	_vuBackupVI(VU, _It_);
	if (_Ft_ != 0)
		VU->VI[_It_].US[0]--;
	u32 addr = (VU->VI[_It_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
}

static __ri void _vuSQI(VURegs* VU)
{
	_vuBackupVI(VU, _It_);
	u32 addr = (VU->VI[_It_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
	if(_Ft_ != 0) VU->VI[_It_].US[0]++;
}

static __ri void _vuILW(VURegs* VU)
{
	if (_It_ == 0)
		return;

	s16 imm  = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);

	if (_X) VU->VI[_It_].US[0] = ptr[0];
	if (_Y) VU->VI[_It_].US[0] = ptr[2];
	if (_Z) VU->VI[_It_].US[0] = ptr[4];
	if (_W) VU->VI[_It_].US[0] = ptr[6];
}

static __fi void _vuISW(VURegs* VU)
{
	s16 imm = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);
	if (_X) { ptr[0] = VU->VI[_It_].US[0]; ptr[1] = 0; }
	if (_Y) { ptr[2] = VU->VI[_It_].US[0]; ptr[3] = 0; }
	if (_Z) { ptr[4] = VU->VI[_It_].US[0]; ptr[5] = 0; }
	if (_W) { ptr[6] = VU->VI[_It_].US[0]; ptr[7] = 0; }
}

static __ri void _vuILWR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);

	if (_X) VU->VI[_It_].US[0] = ptr[0];
	if (_Y) VU->VI[_It_].US[0] = ptr[2];
	if (_Z) VU->VI[_It_].US[0] = ptr[4];
	if (_W) VU->VI[_It_].US[0] = ptr[6];
}

static __ri void _vuISWR(VURegs* VU)
{
	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);
	if (_X) { ptr[0] = VU->VI[_It_].US[0]; ptr[1] = 0; }
	if (_Y) { ptr[2] = VU->VI[_It_].US[0]; ptr[3] = 0; }
	if (_Z) { ptr[4] = VU->VI[_It_].US[0]; ptr[5] = 0; }
	if (_W) { ptr[6] = VU->VI[_It_].US[0]; ptr[7] = 0; }
}

/* code contributed by _Riff_

The following code implements a Galois form M-series LFSR that can be configured to have a width from 0 to 32.
A Galois field can be represented as G(X) = g_m * X^m + g_(m-1) * X^(m-1) + ... + g_1 * X^1 + g0.
A Galois form M-Series LFSR represents a Galois field where g0 = g_m = 1 and the generated set contains 2^M - 1 values.
In modulo-2 arithmetic, addition is replaced by XOR and multiplication is replaced by AND.
The code is written in such a way that the polynomial lsb (g0) should be set to 0 and g_m is not represented.
As an example for setting the polynomial variable correctly, the 23-bit M-series generating polynomial X^23+X^14
  would be specified as (1 << 14).
*/

static __ri void AdvanceLFSR(VURegs* VU)
{
	// code from www.project-fao.org (which is no longer there)
	int x = (VU->VI[REG_R].UL >> 4) & 1;
	int y = (VU->VI[REG_R].UL >> 22) & 1;
	VU->VI[REG_R].UL <<= 1;
	VU->VI[REG_R].UL ^= x ^ y;
	VU->VI[REG_R].UL = (VU->VI[REG_R].UL & 0x7fffff) | 0x3f800000;
}

static __ri void _vuRGET(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].UL[0] = VU->VI[REG_R].UL;
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VI[REG_R].UL;
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VI[REG_R].UL;
	if (_W) VU->VF[_Ft_].UL[3] = VU->VI[REG_R].UL;
}

static __ri void _vuRNEXT(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	AdvanceLFSR(VU);
	if (_X) VU->VF[_Ft_].UL[0] = VU->VI[REG_R].UL;
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VI[REG_R].UL;
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VI[REG_R].UL;
	if (_W) VU->VF[_Ft_].UL[3] = VU->VI[REG_R].UL;
}

static __ri void _vuFSAND(VURegs* VU)
{
	u16 imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) & imm;
}

static __ri void _vuFSEQ(VURegs* VU)
{
	u16 imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;

	if ((VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) == imm)
		VU->VI[_It_].US[0] = 1;
	else
		VU->VI[_It_].US[0] = 0;
}

static __ri void _vuFSOR(VURegs* VU)
{
	u16 imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;
	VU->VI[_It_].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) | imm;
}

static __ri void _vuFSSET(VURegs* VU)
{
	u16 imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7FF);
	VU->statusflag = (imm & 0xFC0) | (VU->statusflag & 0x3F);
}

static __ri void _vuFMAND(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = VU->VI[_Is_].US[0] & (VU->VI[REG_MAC_FLAG].UL & 0xFFFF);
}

static __fi void _vuFMEQ(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if ((VU->VI[REG_MAC_FLAG].UL & 0xFFFF) == VU->VI[_Is_].US[0])
		VU->VI[_It_].US[0] = 1;
	else
		VU->VI[_It_].US[0] = 0;
}

static __fi void _vuFMOR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = (VU->VI[REG_MAC_FLAG].UL & 0xFFFF) | VU->VI[_Is_].US[0];
}

static __fi void _vuFCAND(VURegs* VU)
{
	if ((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) & (VU->code & 0xFFFFFF))
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCEQ(VURegs* VU)
{
	if ((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) == (VU->code & 0xFFFFFF))
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCOR(VURegs* VU)
{
	u32 hold = (VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) | (VU->code & 0xFFFFFF);
	if (hold == 0xFFFFFF)
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCGET(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = VU->VI[REG_CLIP_FLAG].UL & 0x0FFF;
}

s32 _branchAddr(VURegs* VU)
{
	s32 bpc = VU->VI[REG_TPC].SL + (_Imm11_ * 8);
	bpc &= (VU == &vuRegs[1]) ? 0x3fff : 0x0fff;
	return bpc;
}

static __fi void _setBranch(VURegs* VU, u32 bpc)
{
	if (VU->branch == 1)
	{
		VU->delaybranchpc   = bpc;
		VU->takedelaybranch = true;
	}
	else
	{
		VU->branch          = 2;
		VU->branchpc        = bpc;
	}
}

static __ri void _vuIBEQ(VURegs* VU)
{
	s16 dest = VU->VI[_It_].US[0];
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _It_)
			dest = VU->VIOldValue;

		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (dest == src)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBGEZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src >= 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBGTZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif

	if (src > 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBLEZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src <= 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBLTZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src < 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBNE(VURegs* VU)
{
	s16 dest = VU->VI[_It_].US[0];
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _It_)
			dest = VU->VIOldValue;

		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (dest != src)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuB(VURegs* VU)
{
	s32 bpc = _branchAddr(VU);
	_setBranch(VU, bpc);
}

static __ri void _vuBAL(VURegs* VU)
{
	s32 bpc = _branchAddr(VU);


	if (_It_)
	{
		//If we are in the branch delay slot, the instruction after the first
		//instruction in the first branches target becomes the linked reg.
		if (VU->branch == 1)
			VU->VI[_It_].US[0] = (VU->branchpc + 8) / 8;
		else
			VU->VI[_It_].US[0] = (VU->VI[REG_TPC].UL + 8) / 8;
	}

	_setBranch(VU, bpc);
}

static __ri void _vuJR(VURegs* VU)
{
	u32 bpc = VU->VI[_Is_].US[0] * 8;
	_setBranch(VU, bpc);
}

//If this is in a branch delay, the jump isn't taken ( Evil Dead - Fistfull of Boomstick )
static __ri void _vuJALR(VURegs* VU)
{
	u32 bpc = VU->VI[_Is_].US[0] * 8;

	if (_It_)
	{
		//If we are in the branch delay slot, the instruction after the first
		//instruction in the first branches target becomes the linked reg.
		if (VU->branch == 1)
			VU->VI[_It_].US[0] = (VU->branchpc + 8) / 8;
		else
			VU->VI[_It_].US[0] = (VU->VI[REG_TPC].UL + 8) / 8;
	}

	_setBranch(VU, bpc);
}

static __ri void _vuMFP(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].i.x = VU->VI[REG_P].UL;
	if (_Y) VU->VF[_Ft_].i.y = VU->VI[REG_P].UL;
	if (_Z) VU->VF[_Ft_].i.z = VU->VI[REG_P].UL;
	if (_W) VU->VF[_Ft_].i.w = VU->VI[REG_P].UL;
}

static __ri void _vuESADD(VURegs* VU)
{
	VU->p.F = vuDouble(VU->VF[_Fs_].i.x) 
		* vuDouble(VU->VF[_Fs_].i.x) 
		+ vuDouble(VU->VF[_Fs_].i.y) 
		* vuDouble(VU->VF[_Fs_].i.y) 
		+ vuDouble(VU->VF[_Fs_].i.z) 
		* vuDouble(VU->VF[_Fs_].i.z);
}

static __ri void _vuERSADD(VURegs* VU)
{
	float p = (vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x)) + (vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y)) + (vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z));

	if (p != 0.0)
		p = 1.0f / p;

	VU->p.F = p;
}

static __ri void _vuELENG(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z);

	if (p >= 0)
		p = sqrt(p);
	VU->p.F = p;
}

static __ri void _vuERLENG(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z);

	if (p >= 0)
	{
		p = sqrt(p);
		if (p != 0)
			p = 1.0f / p;
	}
	VU->p.F = p;
}


static __ri float _vuCalculateEATAN(float inputvalue)
{
	float eatanconst[9] = {  0.999999344348907f,
				-0.333298563957214f,
				 0.199465364217758f,
				-0.13085337519646f,
				 0.096420042216778f,
				-0.055909886956215f,
				 0.021861229091883f,
				-0.004054057877511f,
				 0.785398185253143f };
	float result = (eatanconst[0] * inputvalue) 
		     + (eatanconst[1] * pow(inputvalue, 3)) 
		     + (eatanconst[2] * pow(inputvalue, 5))
		     + (eatanconst[3] * pow(inputvalue, 7)) 
		     + (eatanconst[4] * pow(inputvalue, 9)) 
		     + (eatanconst[5] * pow(inputvalue, 11))
		     + (eatanconst[6] * pow(inputvalue, 13)) 
		     + (eatanconst[7] * pow(inputvalue, 15));

	result += eatanconst[8];
	return vuDouble(*(u32*)&result);
}

static __ri void _vuEATAN(VURegs* VU)
{
	VU->p.F = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].UL[_Fsf_]));
}

static __ri void _vuEATANxy(VURegs* VU)
{
	float p = 0;
	if (vuDouble(VU->VF[_Fs_].i.x) != 0)
		p = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].i.y) / vuDouble(VU->VF[_Fs_].i.x));
	VU->p.F = p;
}

static __ri void _vuEATANxz(VURegs* VU)
{
	float p = 0;
	if (vuDouble(VU->VF[_Fs_].i.x) != 0)
		p = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].i.z) / vuDouble(VU->VF[_Fs_].i.x));
	VU->p.F = p;
}

static __ri void _vuESUM(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VF[_Fs_].i.w);
	VU->p.F = p;
}

static __ri void _vuERCPR(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p != 0)
		p = 1.0 / p;

	VU->p.F = p;
}

static __ri void _vuESQRT(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p >= 0)
		p = sqrt(p);

	VU->p.F = p;
}

static __ri void _vuERSQRT(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p >= 0)
	{
		p = sqrt(p);
		if (p)
			p = 1.0f / p;
	}

	VU->p.F = p;
}

static __ri void _vuESIN(VURegs* VU)
{
	float sinconsts[5] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	p = (sinconsts[0] * p) + (sinconsts[1] * pow(p, 3)) + (sinconsts[2] * pow(p, 5)) + (sinconsts[3] * pow(p, 7)) + (sinconsts[4] * pow(p, 9));
	VU->p.F = vuDouble(*(u32*)&p);
}

static __ri void _vuEEXP(VURegs* VU)
{
	float consts[6] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
						0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	p = 1.0f + (consts[0] * p) + (consts[1] * pow(p, 2)) + (consts[2] * pow(p, 3)) + (consts[3] * pow(p, 4)) + (consts[4] * pow(p, 5)) + (consts[5] * pow(p, 6));
	p = pow(p, 4);
	p = vuDouble(*(u32*)&p);
	p = 1 / p;

	VU->p.F = p;
}

static __ri void _vuXITOP(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if (VU == &vuRegs[1] && THREAD_VU1)
		VU->VI[_It_].US[0] = vu1Thread.vifRegs.itop;
	else
		VU->VI[_It_].US[0] = (VU == &vuRegs[1]) ? vif1Regs.itop : vif0Regs.itop;
}

void _vuXGKICKTransfer(s32 cycles, bool flush)
{
	if (!vuRegs[1].xgkickenable)
		return;

	vuRegs[1].xgkickcyclecount += cycles;
	vuRegs[1].xgkicklastcycle += cycles;

	while (vuRegs[1].xgkickenable && (flush || vuRegs[1].xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (vuRegs[1].xgkicksizeremaining == 0)
		{
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, vuRegs[1].Mem, vuRegs[1].xgkickaddr, ~0u, flush);
			vuRegs[1].xgkicksizeremaining = size & 0xFFFF;
			vuRegs[1].xgkickendpacket     = size >> 31;
			vuRegs[1].xgkickdiff          = 0x4000 - vuRegs[1].xgkickaddr;

			if (vuRegs[1].xgkicksizeremaining == 0)
			{
				vuRegs[1].xgkickenable = false;
				break;
			}
		}

		if (!flush)
		{
			transfersize = std::min(vuRegs[1].xgkicksizeremaining / 0x10, vuRegs[1].xgkickcyclecount / 2);
			transfersize = std::min(transfersize, vuRegs[1].xgkickdiff / 0x10);
		}
		else
		{
			transfersize = vuRegs[1].xgkicksizeremaining / 0x10;
			transfersize = std::min(transfersize, vuRegs[1].xgkickdiff / 0x10);
		}

		// Would be "nicer" to do the copy until it's all up, 
		// however this really screws up PATH3 masking stuff
		// So lets just do it the other way :)
		/*if (THREAD_VU1)
		{
			if ((transfersize * 0x10) < vuRegs[1].xgkicksizeremaining)
				gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize * 0x10, true);
			else
				gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize * 0x10, true);
		}
		else*/
		//{
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize * 0x10, true);
		//}

		if ((vuRegs[0].VI[REG_VPU_STAT].UL & 0x100) && flush)
			vuRegs[1].cycle += transfersize * 2;

		vuRegs[1].xgkickcyclecount -= transfersize * 2;

		vuRegs[1].xgkickaddr = (vuRegs[1].xgkickaddr + (transfersize * 0x10)) & 0x3FFF;
		vuRegs[1].xgkicksizeremaining -= (transfersize * 0x10);
		vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

		if (vuRegs[1].xgkicksizeremaining || !vuRegs[1].xgkickendpacket) { }
		else
		{
			vuRegs[1].xgkickenable = false;
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~(1 << 12);
			// Check if VIF is waiting for the GIF to not be busy
			if (vif1Regs.stat.VGW)
			{
				vif1Regs.stat.VGW = false;
				CPU_INT(DMAC_VIF1, 8);
			}
		}
	}
	if (flush)
	{
		_vuTestPipes(&vuRegs[1]);
	}
}

static __ri void _vuXGKICK(VURegs* VU)
{
	if (VU->xgkickenable)
		_vuXGKICKTransfer(0, true);

	u32 addr                 = (VU->VI[_Is_].US[0] & 0x3ff) * 16;
	u32 diff                 = 0x4000 - addr;

	VU->xgkickenable         = true;
	VU->xgkickaddr           = addr;
	VU->xgkickdiff           = diff;
	VU->xgkicksizeremaining  = 0;
	VU->xgkickendpacket      = false;
	VU->xgkicklastcycle      = VU->cycle;
	// XGKick command counts as one cycle for the transfer.
	// Can be tested with Resident Evil: Outbreak, Kingdom Hearts, CART Fury.
	VU->xgkickcyclecount     = 1;
	vuRegs[0].VI[REG_VPU_STAT].UL |= (1 << 12);
}

static __ri void _vuXTOP(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if (VU == &vuRegs[1] && THREAD_VU1)
		VU->VI[_It_].US[0] = (u16)vu1Thread.vifRegs.top;
	else
		VU->VI[_It_].US[0] = (VU == &vuRegs[1]) ? (u16)vif1Regs.top : (u16)vif0Regs.top;
}

#define GET_VF0_FLAG(reg) (((reg) == 0) ? (1 << REG_VF0_FLAG) : 0)

#define VUREGS_FDFSI(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1 << REG_I)|((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_); \
}

#define VUREGS_FDFSQ(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1 << REG_Q)|((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_); \
}

#define VUREGS_FDFSFT(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= _XYZW; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = ((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_); \
}

#define VUREGS_FDFSFTxyzw(OP, xyzw, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = ((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_); \
}

#define VUREGS_FDFSFTx(OP, ACC) VUREGS_FDFSFTxyzw(OP, 8, ACC)
#define VUREGS_FDFSFTy(OP, ACC) VUREGS_FDFSFTxyzw(OP, 4, ACC)
#define VUREGS_FDFSFTz(OP, ACC) VUREGS_FDFSFTxyzw(OP, 2, ACC)
#define VUREGS_FDFSFTw(OP, ACC) VUREGS_FDFSFTxyzw(OP, 1, ACC)


#define VUREGS_ACCFSI(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = (1 << REG_I)|GET_VF0_FLAG(_Fs_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSQ(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = (1 << REG_Q)|GET_VF0_FLAG(_Fs_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFT(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= _XYZW; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFTxyzw(OP, xyzw, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFTx(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 8, readacc)
#define VUREGS_ACCFSFTy(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 4, readacc)
#define VUREGS_ACCFSFTz(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 2, readacc)
#define VUREGS_ACCFSFTw(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 1, readacc)

#define VUREGS_FTFS(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Ft_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VFr1xyzw = 0xff; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0); \
}

#define VUREGS_IDISIT(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_IALU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = 0; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 1 << _Id_; \
	VUregsn->VIread  = (1 << _Is_) | (1 << _It_); \
	VUregsn->cycles  = 0; \
}

#define VUREGS_ITIS(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_IALU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = 0; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 1 << _It_; \
	VUregsn->VIread  = 1 << _Is_; \
	VUregsn->cycles  = 0; \
}

#define VUREGS_PFS_xyzw(OP, _cycles) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_EFU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
    VUregsn->VIwrite = 1 << REG_P; \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_); \
	VUregsn->cycles  = _cycles; \
}

#define VUREGS_PFS_fsf(OP, _cycles) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_EFU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= 1 << (3-_Fsf_); \
	VUregsn->VFread1 = 0; \
    VUregsn->VIwrite = 1 << REG_P; \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_); \
	VUregsn->cycles  = _cycles; \
}

VUREGS_FTFS(ABS);

VUREGS_FDFSFT(ADD, 0);
VUREGS_FDFSI(ADDi, 0);
VUREGS_FDFSQ(ADDq, 0);
VUREGS_FDFSFTx(ADDx, 0);
VUREGS_FDFSFTy(ADDy, 0);
VUREGS_FDFSFTz(ADDz, 0);
VUREGS_FDFSFTw(ADDw, 0);

VUREGS_ACCFSFT(ADDA, 0);
VUREGS_ACCFSI(ADDAi, 0);
VUREGS_ACCFSQ(ADDAq, 0);
VUREGS_ACCFSFTx(ADDAx, 0);
VUREGS_ACCFSFTy(ADDAy, 0);
VUREGS_ACCFSFTz(ADDAz, 0);
VUREGS_ACCFSFTw(ADDAw, 0);

VUREGS_FDFSFT(SUB, 0);
VUREGS_FDFSI(SUBi, 0);
VUREGS_FDFSQ(SUBq, 0);
VUREGS_FDFSFTx(SUBx, 0);
VUREGS_FDFSFTy(SUBy, 0);
VUREGS_FDFSFTz(SUBz, 0);
VUREGS_FDFSFTw(SUBw, 0);

VUREGS_ACCFSFT(SUBA, 0);
VUREGS_ACCFSI(SUBAi, 0);
VUREGS_ACCFSQ(SUBAq, 0);
VUREGS_ACCFSFTx(SUBAx, 0);
VUREGS_ACCFSFTy(SUBAy, 0);
VUREGS_ACCFSFTz(SUBAz, 0);
VUREGS_ACCFSFTw(SUBAw, 0);

#define VUREGS_FDFSFTxyzw_MUL(OP, ACC, xyzw) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
		VUregsn->pipe = VUPIPE_FMAC; \
		VUregsn->VFwrite = (ACC)?0:_Fd_; \
		VUregsn->VFwxyzw = _XYZW; \
		VUregsn->VFread0 = _Fs_; \
		VUregsn->VFr0xyzw= _XYZW; \
		VUregsn->VFread1 = _Ft_; \
		VUregsn->VFr1xyzw= xyzw; \
		VUregsn->VIwrite = ((ACC)?(1<<REG_ACC_FLAG):0); \
		VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|(((ACC)&&(_XYZW!=15))?(1<<REG_ACC_FLAG):0); \
}

VUREGS_FDFSFT(MUL, 0);
VUREGS_FDFSI(MULi, 0);
VUREGS_FDFSQ(MULq, 0);
VUREGS_FDFSFTxyzw_MUL(MULx, 0, 8);
VUREGS_FDFSFTxyzw_MUL(MULy, 0, 4);
VUREGS_FDFSFTxyzw_MUL(MULz, 0, 2);
VUREGS_FDFSFTxyzw_MUL(MULw, 0, 1);

VUREGS_ACCFSFT(MULA, 0);
VUREGS_ACCFSI(MULAi, 0);
VUREGS_ACCFSQ(MULAq, 0);
VUREGS_FDFSFTxyzw_MUL(MULAx, 1, 8);
VUREGS_FDFSFTxyzw_MUL(MULAy, 1, 4);
VUREGS_FDFSFTxyzw_MUL(MULAz, 1, 2);
VUREGS_FDFSFTxyzw_MUL(MULAw, 1, 1);

VUREGS_FDFSFT(MADD, 1);
VUREGS_FDFSI(MADDi, 1);
VUREGS_FDFSQ(MADDq, 1);

#define VUREGS_FDFSFT_0_xyzw(OP, xyzw) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1<<REG_ACC_FLAG)|(_Ft_ ? GET_VF0_FLAG(_Fs_) : 0); \
}

VUREGS_FDFSFT_0_xyzw(MADDx, 8);
VUREGS_FDFSFT_0_xyzw(MADDy, 4);
VUREGS_FDFSFT_0_xyzw(MADDz, 2);

static __ri void _vuRegsMADDw(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FMAC;
   VUregsn->VFwrite   = _Fd_;
   VUregsn->VFwxyzw   = _XYZW;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = _XYZW;
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 1;
   VUregsn->VIwrite   = 0;
   VUregsn->VIread    = (1<<REG_ACC_FLAG)|GET_VF0_FLAG(_Fs_);
}

VUREGS_ACCFSFT(MADDA, 1);
VUREGS_ACCFSI(MADDAi, 1);
VUREGS_ACCFSQ(MADDAq, 1);
VUREGS_ACCFSFTx(MADDAx, 1);
VUREGS_ACCFSFTy(MADDAy, 1);
VUREGS_ACCFSFTz(MADDAz, 1);
VUREGS_ACCFSFTw(MADDAw, 1);

VUREGS_FDFSFT(MSUB, 1);
VUREGS_FDFSI(MSUBi, 1);
VUREGS_FDFSQ(MSUBq, 1);
VUREGS_FDFSFTx(MSUBx, 1);
VUREGS_FDFSFTy(MSUBy, 1);
VUREGS_FDFSFTz(MSUBz, 1);
VUREGS_FDFSFTw(MSUBw, 1);

VUREGS_ACCFSFT(MSUBA, 1);
VUREGS_ACCFSI(MSUBAi, 1);
VUREGS_ACCFSQ(MSUBAq, 1);
VUREGS_ACCFSFTx(MSUBAx, 1);
VUREGS_ACCFSFTy(MSUBAy, 1);
VUREGS_ACCFSFTz(MSUBAz, 1);
VUREGS_ACCFSFTw(MSUBAw, 1);

VUREGS_FDFSFT(MAX, 0);
VUREGS_FDFSI(MAXi, 0);
VUREGS_FDFSFTx(MAXx_, 0);
VUREGS_FDFSFTy(MAXy_, 0);
VUREGS_FDFSFTz(MAXz_, 0);
VUREGS_FDFSFTw(MAXw_, 0);

VUREGS_FDFSFT(MINI, 0);
VUREGS_FDFSI(MINIi, 0);
VUREGS_FDFSFTx(MINIx, 0);
VUREGS_FDFSFTy(MINIy, 0);
VUREGS_FDFSFTz(MINIz, 0);
VUREGS_FDFSFTw(MINIw, 0);

static __ri void _vuRegsOPMULA(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FMAC;
   VUregsn->VFwrite   = 0;
   VUregsn->VFwxyzw   = 0xE;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = 0xE;
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 0xE;
   VUregsn->VIwrite   = 1<<REG_ACC_FLAG;
   VUregsn->VIread    = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1<<REG_ACC_FLAG);
}

static __ri void _vuRegsOPMSUB(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FMAC;
   VUregsn->VFwrite   = _Fd_;
   VUregsn->VFwxyzw   = 0xE;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = 0xE;
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 0xE;
   VUregsn->VIwrite   = 0;
   VUregsn->VIread    = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1<<REG_ACC_FLAG);
}

static __ri void _vuRegsNOP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_NONE;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 0;
}

VUREGS_FTFS(FTOI0);
VUREGS_FTFS(FTOI4);
VUREGS_FTFS(FTOI12);
VUREGS_FTFS(FTOI15);
VUREGS_FTFS(ITOF0);
VUREGS_FTFS(ITOF4);
VUREGS_FTFS(ITOF12);
VUREGS_FTFS(ITOF15);

static __ri void _vuRegsCLIP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = 0xE;
    VUregsn->VFread1  = _Ft_;
    VUregsn->VFr1xyzw = 0x1;
    VUregsn->VIwrite  = 1 << REG_CLIP_FLAG;
    VUregsn->VIread   = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1 << REG_CLIP_FLAG);
}

/******************************/
/*   VU Lower instructions    */
/******************************/

static __ri void _vuRegsDIV(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FDIV;
   VUregsn->VFwrite   = 0;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = 1 << (3-_Fsf_);
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 1 << (3-_Ftf_);
   VUregsn->VIwrite   = 1 << REG_Q;
   VUregsn->VIread    = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_);
   VUregsn->cycles    = 7;
}

static __ri void _vuRegsSQRT(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FDIV;
   VUregsn->VFwrite   = 0;
   VUregsn->VFread0   = 0;
   VUregsn->VFr0xyzw  = 0;
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 1 << (3-_Ftf_);
   VUregsn->VIwrite   = 1 << REG_Q;
   VUregsn->VIread    = GET_VF0_FLAG(_Ft_);
   VUregsn->cycles    = 7;
}

static __ri void _vuRegsRSQRT(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FDIV;
   VUregsn->VFwrite   = 0;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = 1 << (3-_Fsf_);
   VUregsn->VFread1   = _Ft_;
   VUregsn->VFr1xyzw  = 1 << (3-_Ftf_);
   VUregsn->VIwrite   = 1 << REG_Q;
   VUregsn->VIread    = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_);
   VUregsn->cycles    = 13;
}

VUREGS_ITIS(IADDI);
VUREGS_ITIS(IADDIU);
VUREGS_IDISIT(IADD);
VUREGS_IDISIT(IAND);
VUREGS_IDISIT(IOR);
VUREGS_IDISIT(ISUB);
VUREGS_ITIS(ISUBIU);

static __ri void _vuRegsMOVE(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = _Ft_ == 0 ? VUPIPE_NONE : VUPIPE_FMAC;
   VUregsn->VFwrite   = _Ft_;
   VUregsn->VFwxyzw   = _XYZW;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = _XYZW;
   VUregsn->VFread1   = 0;
   VUregsn->VFr1xyzw  = 0;
   VUregsn->VIwrite   = 0;
   VUregsn->VIread    = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0);
}
static __ri void _vuRegsMFIR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 1 << _Is_;
}

static __ri void _vuRegsMTIR(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FMAC;
   VUregsn->VFwrite   = 0;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = 1 << (3-_Fsf_);
   VUregsn->VFread1   = 0;
   VUregsn->VIwrite   = 1 << _It_;
   VUregsn->VIread    = GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsMR32(const VURegs* VU, _VURegsNum* VUregsn)
{
   VUregsn->pipe      = VUPIPE_FMAC;
   VUregsn->VFwrite   = _Ft_;
   VUregsn->VFwxyzw   = _XYZW;
   VUregsn->VFread0   = _Fs_;
   VUregsn->VFr0xyzw  = (_XYZW >> 1) | ((_XYZW << 3) & 0x8);  //rotate
   VUregsn->VFread1   = 0;
   VUregsn->VFr1xyzw  = 0xff;
   VUregsn->VIwrite   = 0;
   VUregsn->VIread    = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0);
}

static __ri void _vuRegsLQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 1 << _Is_;
}

static __ri void _vuRegsLQD(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << _Is_;
    VUregsn->VIread   = 1 << _Is_;
}

static __ri void _vuRegsLQI(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << _Is_;
    VUregsn->VIread   = 1 << _Is_;
}

static __ri void _vuRegsSQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = _XYZW;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 1 << _It_;
}

static __ri void _vuRegsSQD(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = _XYZW;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << _It_;
    VUregsn->VIread   = 1 << _It_;
}

static __ri void _vuRegsSQI(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = _XYZW;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << _It_;
    VUregsn->VIread   = 1 << _It_;
}

static __ri void _vuRegsILW(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_IALU;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << _It_;
    VUregsn->VIread   = 1 << _Is_;
    VUregsn->cycles   = 4;
}

static __ri void _vuRegsISW(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_IALU;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsILWR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_IALU;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = (1 << _It_);
    VUregsn->VIread   = (1 << _Is_);
    VUregsn->cycles   = 4;
}

static __ri void _vuRegsISWR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_IALU;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsRINIT(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = 1 << (3-_Fsf_);
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << REG_R;
    VUregsn->VIread   = GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsRGET(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 1 << REG_R;
}

static __ri void _vuRegsRNEXT(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = _Ft_;
    VUregsn->VFwxyzw  = _XYZW;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << REG_R;
    VUregsn->VIread   = 1 << REG_R;
}

static __ri void _vuRegsRXOR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FMAC;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = _Fs_;
    VUregsn->VFr0xyzw = 1 << (3-_Fsf_);
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 1 << REG_R;
    VUregsn->VIread   = (1 << REG_R)|GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsWAITQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe     = VUPIPE_FDIV;
    VUregsn->VFwrite  = 0;
    VUregsn->VFread0  = 0;
    VUregsn->VFread1  = 0;
    VUregsn->VIwrite  = 0;
    VUregsn->VIread   = 0;
}

static __ri void _vuRegsFSAND(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSOR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSSET(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_STATUS_FLAG;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsFMAND(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFMEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFMOR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFCAND(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCOR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCSET(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_CLIP_FLAG;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsFCGET(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsIBEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsIBGEZ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBGTZ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBLEZ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBLTZ(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBNE(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsB(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsBAL(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsJR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsJALR(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsMFP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << REG_P;
}

static __ri void _vuRegsWAITP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_EFU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
    VUregsn->cycles  = 0;
}

VUREGS_PFS_xyzw(ESADD, 11);
VUREGS_PFS_xyzw(ERSADD, 18);
VUREGS_PFS_xyzw(ELENG, 18);
VUREGS_PFS_xyzw(ERLENG, 24);
VUREGS_PFS_xyzw(EATANxy, 54);
VUREGS_PFS_xyzw(EATANxz, 54);
VUREGS_PFS_xyzw(ESUM, 12);
VUREGS_PFS_fsf(ERCPR, 12);
VUREGS_PFS_fsf(ESQRT, 12);
VUREGS_PFS_fsf(ERSQRT, 18);
VUREGS_PFS_fsf(ESIN, 29);
VUREGS_PFS_fsf(EATAN, 54);
VUREGS_PFS_fsf(EEXP, 44);

static __ri void _vuRegsXITOP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
    VUregsn->cycles  = 0;
}

static __ri void _vuRegsXGKICK(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_XGKICK;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsXTOP(const VURegs* VU, _VURegsNum* VUregsn)
{
    VUregsn->pipe    = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
    VUregsn->cycles  = 0;
}

// --------------------------------------------------------------------------------------
//  VU0
// --------------------------------------------------------------------------------------

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU0MI_ABS()  { _vuABS(&vuRegs[0]); }
static void VU0MI_ADD()  { _vuADD(&vuRegs[0]); }
static void VU0MI_ADDi() { _vuADDi(&vuRegs[0]); }
static void VU0MI_ADDq() { _vuADDq(&vuRegs[0]); }
static void VU0MI_ADDx() { _vuADDx(&vuRegs[0]); }
static void VU0MI_ADDy() { _vuADDy(&vuRegs[0]); }
static void VU0MI_ADDz() { _vuADDz(&vuRegs[0]); }
static void VU0MI_ADDw() { _vuADDw(&vuRegs[0]); }
static void VU0MI_ADDA() { _vuADDA(&vuRegs[0]); }
static void VU0MI_ADDAi() { _vuADDAi(&vuRegs[0]); }
static void VU0MI_ADDAq() { _vuADDAq(&vuRegs[0]); }
static void VU0MI_ADDAx() { _vuADDAx(&vuRegs[0]); }
static void VU0MI_ADDAy() { _vuADDAy(&vuRegs[0]); }
static void VU0MI_ADDAz() { _vuADDAz(&vuRegs[0]); }
static void VU0MI_ADDAw() { _vuADDAw(&vuRegs[0]); }
static void VU0MI_SUB()  { _vuSUB(&vuRegs[0]); }
static void VU0MI_SUBi() { _vuSUBi(&vuRegs[0]); }
static void VU0MI_SUBq() { _vuSUBq(&vuRegs[0]); }
static void VU0MI_SUBx() { _vuSUBx(&vuRegs[0]); }
static void VU0MI_SUBy() { _vuSUBy(&vuRegs[0]); }
static void VU0MI_SUBz() { _vuSUBz(&vuRegs[0]); }
static void VU0MI_SUBw() { _vuSUBw(&vuRegs[0]); }
static void VU0MI_SUBA()  { _vuSUBA(&vuRegs[0]); }
static void VU0MI_SUBAi() { _vuSUBAi(&vuRegs[0]); }
static void VU0MI_SUBAq() { _vuSUBAq(&vuRegs[0]); }
static void VU0MI_SUBAx() { _vuSUBAx(&vuRegs[0]); }
static void VU0MI_SUBAy() { _vuSUBAy(&vuRegs[0]); }
static void VU0MI_SUBAz() { _vuSUBAz(&vuRegs[0]); }
static void VU0MI_SUBAw() { _vuSUBAw(&vuRegs[0]); }
static void VU0MI_MUL()  { _vuMUL(&vuRegs[0]); }
static void VU0MI_MULi() { _vuMULi(&vuRegs[0]); }
static void VU0MI_MULq() { _vuMULq(&vuRegs[0]); }
static void VU0MI_MULx() { _vuMULx(&vuRegs[0]); }
static void VU0MI_MULy() { _vuMULy(&vuRegs[0]); }
static void VU0MI_MULz() { _vuMULz(&vuRegs[0]); }
static void VU0MI_MULw() { _vuMULw(&vuRegs[0]); }
static void VU0MI_MULA()  { _vuMULA(&vuRegs[0]); }
static void VU0MI_MULAi() { _vuMULAi(&vuRegs[0]); }
static void VU0MI_MULAq() { _vuMULAq(&vuRegs[0]); }
static void VU0MI_MULAx() { _vuMULAx(&vuRegs[0]); }
static void VU0MI_MULAy() { _vuMULAy(&vuRegs[0]); }
static void VU0MI_MULAz() { _vuMULAz(&vuRegs[0]); }
static void VU0MI_MULAw() { _vuMULAw(&vuRegs[0]); }
static void VU0MI_MADD()  { _vuMADD(&vuRegs[0]); }
static void VU0MI_MADDi() { _vuMADDi(&vuRegs[0]); }
static void VU0MI_MADDq() { _vuMADDq(&vuRegs[0]); }
static void VU0MI_MADDx() { _vuMADDx(&vuRegs[0]); }
static void VU0MI_MADDy() { _vuMADDy(&vuRegs[0]); }
static void VU0MI_MADDz() { _vuMADDz(&vuRegs[0]); }
static void VU0MI_MADDw() { _vuMADDw(&vuRegs[0]); }
static void VU0MI_MADDA()  { _vuMADDA(&vuRegs[0]); }
static void VU0MI_MADDAi() { _vuMADDAi(&vuRegs[0]); }
static void VU0MI_MADDAq() { _vuMADDAq(&vuRegs[0]); }
static void VU0MI_MADDAx() { _vuMADDAx(&vuRegs[0]); }
static void VU0MI_MADDAy() { _vuMADDAy(&vuRegs[0]); }
static void VU0MI_MADDAz() { _vuMADDAz(&vuRegs[0]); }
static void VU0MI_MADDAw() { _vuMADDAw(&vuRegs[0]); }
static void VU0MI_MSUB()  { _vuMSUB(&vuRegs[0]); }
static void VU0MI_MSUBi() { _vuMSUBi(&vuRegs[0]); }
static void VU0MI_MSUBq() { _vuMSUBq(&vuRegs[0]); }
static void VU0MI_MSUBx() { _vuMSUBx(&vuRegs[0]); }
static void VU0MI_MSUBy() { _vuMSUBy(&vuRegs[0]); }
static void VU0MI_MSUBz() { _vuMSUBz(&vuRegs[0]); }
static void VU0MI_MSUBw() { _vuMSUBw(&vuRegs[0]); }
static void VU0MI_MSUBA()  { _vuMSUBA(&vuRegs[0]); }
static void VU0MI_MSUBAi() { _vuMSUBAi(&vuRegs[0]); }
static void VU0MI_MSUBAq() { _vuMSUBAq(&vuRegs[0]); }
static void VU0MI_MSUBAx() { _vuMSUBAx(&vuRegs[0]); }
static void VU0MI_MSUBAy() { _vuMSUBAy(&vuRegs[0]); }
static void VU0MI_MSUBAz() { _vuMSUBAz(&vuRegs[0]); }
static void VU0MI_MSUBAw() { _vuMSUBAw(&vuRegs[0]); }
static void VU0MI_MAX()  { _vuMAX(&vuRegs[0]); }
static void VU0MI_MAXi() { _vuMAXi(&vuRegs[0]); }
static void VU0MI_MAXx() { _vuMAXx(&vuRegs[0]); }
static void VU0MI_MAXy() { _vuMAXy(&vuRegs[0]); }
static void VU0MI_MAXz() { _vuMAXz(&vuRegs[0]); }
static void VU0MI_MAXw() { _vuMAXw(&vuRegs[0]); }
static void VU0MI_MINI()  { _vuMINI(&vuRegs[0]); }
static void VU0MI_MINIi() { _vuMINIi(&vuRegs[0]); }
static void VU0MI_MINIx() { _vuMINIx(&vuRegs[0]); }
static void VU0MI_MINIy() { _vuMINIy(&vuRegs[0]); }
static void VU0MI_MINIz() { _vuMINIz(&vuRegs[0]); }
static void VU0MI_MINIw() { _vuMINIw(&vuRegs[0]); }
static void VU0MI_OPMULA() { _vuOPMULA(&vuRegs[0]); }
static void VU0MI_OPMSUB() { _vuOPMSUB(&vuRegs[0]); }
static void VU0MI_NOP() { _vuNOP(&vuRegs[0]); }
static void VU0MI_FTOI0()  { _vuFTOI0(&vuRegs[0]); }
static void VU0MI_FTOI4()  { _vuFTOI4(&vuRegs[0]); }
static void VU0MI_FTOI12() { _vuFTOI12(&vuRegs[0]); }
static void VU0MI_FTOI15() { _vuFTOI15(&vuRegs[0]); }
static void VU0MI_ITOF0()  { _vuITOF0(&vuRegs[0]); }
static void VU0MI_ITOF4()  { _vuITOF4(&vuRegs[0]); }
static void VU0MI_ITOF12() { _vuITOF12(&vuRegs[0]); }
static void VU0MI_ITOF15() { _vuITOF15(&vuRegs[0]); }
static void VU0MI_CLIP() { _vuCLIP(&vuRegs[0]); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU0MI_DIV() { _vuDIV(&vuRegs[0]); }
static void VU0MI_SQRT() { _vuSQRT(&vuRegs[0]); }
static void VU0MI_RSQRT() { _vuRSQRT(&vuRegs[0]); }
static void VU0MI_IADD() { _vuIADD(&vuRegs[0]); }
static void VU0MI_IADDI() { _vuIADDI(&vuRegs[0]); }
static void VU0MI_IADDIU() { _vuIADDIU(&vuRegs[0]); }
static void VU0MI_IAND() { _vuIAND(&vuRegs[0]); }
static void VU0MI_IOR() { _vuIOR(&vuRegs[0]); }
static void VU0MI_ISUB() { _vuISUB(&vuRegs[0]); }
static void VU0MI_ISUBIU() { _vuISUBIU(&vuRegs[0]); }
static void VU0MI_MOVE() { _vuMOVE(&vuRegs[0]); }
static void VU0MI_MFIR() { _vuMFIR(&vuRegs[0]); }
static void VU0MI_MTIR() { _vuMTIR(&vuRegs[0]); }
static void VU0MI_MR32() { _vuMR32(&vuRegs[0]); }
static void VU0MI_LQ() { _vuLQ(&vuRegs[0]); }
static void VU0MI_LQD() { _vuLQD(&vuRegs[0]); }
static void VU0MI_LQI() { _vuLQI(&vuRegs[0]); }
static void VU0MI_SQ() { _vuSQ(&vuRegs[0]); }
static void VU0MI_SQD() { _vuSQD(&vuRegs[0]); }
static void VU0MI_SQI() { _vuSQI(&vuRegs[0]); }
static void VU0MI_ILW() { _vuILW(&vuRegs[0]); }
static void VU0MI_ISW() { _vuISW(&vuRegs[0]); }
static void VU0MI_ILWR() { _vuILWR(&vuRegs[0]); }
static void VU0MI_ISWR() { _vuISWR(&vuRegs[0]); }
static void VU0MI_RINIT() { vuRegs[0].VI[REG_R].UL = 0x3F800000 | (vuRegs[0].VF[((vuRegs[0].code >> 11) & 0x1F)].UL[((vuRegs[0].code >> 21) & 0x03)] & 0x007FFFFF); }
static void VU0MI_RGET()  { _vuRGET(&vuRegs[0]); }
static void VU0MI_RNEXT() { _vuRNEXT(&vuRegs[0]); }
static void VU0MI_RXOR()  { vuRegs[0].VI[REG_R].UL = 0x3F800000 | ((vuRegs[0].VI[REG_R].UL ^ vuRegs[0].VF[((vuRegs[0].code >> 11) & 0x1F)].UL[((vuRegs[0].code >> 21) & 0x03)]) & 0x007FFFFF); }
static void VU0MI_WAITQ() { }
static void VU0MI_FSAND() { _vuFSAND(&vuRegs[0]); }
static void VU0MI_FSEQ()  { _vuFSEQ(&vuRegs[0]); }
static void VU0MI_FSOR()  { _vuFSOR(&vuRegs[0]); }
static void VU0MI_FSSET() { _vuFSSET(&vuRegs[0]); }
static void VU0MI_FMAND() { _vuFMAND(&vuRegs[0]); }
static void VU0MI_FMEQ()  { _vuFMEQ(&vuRegs[0]); }
static void VU0MI_FMOR()  { _vuFMOR(&vuRegs[0]); }
static void VU0MI_FCAND() { _vuFCAND(&vuRegs[0]); }
static void VU0MI_FCEQ()  { _vuFCEQ(&vuRegs[0]); }
static void VU0MI_FCOR()  { _vuFCOR(&vuRegs[0]); }
static void VU0MI_FCSET() { vuRegs[0].clipflag = (u32)(vuRegs[0].code & 0xFFFFFF); }
static void VU0MI_FCGET() { _vuFCGET(&vuRegs[0]); }
static void VU0MI_IBEQ() { _vuIBEQ(&vuRegs[0]); }
static void VU0MI_IBGEZ() { _vuIBGEZ(&vuRegs[0]); }
static void VU0MI_IBGTZ() { _vuIBGTZ(&vuRegs[0]); }
static void VU0MI_IBLTZ() { _vuIBLTZ(&vuRegs[0]); }
static void VU0MI_IBLEZ() { _vuIBLEZ(&vuRegs[0]); }
static void VU0MI_IBNE() { _vuIBNE(&vuRegs[0]); }
static void VU0MI_B()   { _vuB(&vuRegs[0]); }
static void VU0MI_BAL() { _vuBAL(&vuRegs[0]); }
static void VU0MI_JR()   { _vuJR(&vuRegs[0]); }
static void VU0MI_JALR() { _vuJALR(&vuRegs[0]); }
static void VU0MI_MFP() { _vuMFP(&vuRegs[0]); }
static void VU0MI_WAITP() { }
static void VU0MI_ESADD()   { _vuESADD(&vuRegs[0]); }
static void VU0MI_ERSADD()  { _vuERSADD(&vuRegs[0]); }
static void VU0MI_ELENG()   { _vuELENG(&vuRegs[0]); }
static void VU0MI_ERLENG()  { _vuERLENG(&vuRegs[0]); }
static void VU0MI_EATANxy() { _vuEATANxy(&vuRegs[0]); }
static void VU0MI_EATANxz() { _vuEATANxz(&vuRegs[0]); }
static void VU0MI_ESUM()    { _vuESUM(&vuRegs[0]); }
static void VU0MI_ERCPR()   { _vuERCPR(&vuRegs[0]); }
static void VU0MI_ESQRT()   { _vuESQRT(&vuRegs[0]); }
static void VU0MI_ERSQRT()  { _vuERSQRT(&vuRegs[0]); }
static void VU0MI_ESIN()    { _vuESIN(&vuRegs[0]); }
static void VU0MI_EATAN()   { _vuEATAN(&vuRegs[0]); }
static void VU0MI_EEXP()    { _vuEEXP(&vuRegs[0]); }
static void VU0MI_XITOP() { _vuXITOP(&vuRegs[0]); }
static void VU0MI_XGKICK() {}
static void VU0MI_XTOP() {}

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU0regsMI_ABS(_VURegsNum* VUregsn) { _vuRegsABS(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADD(_VURegsNum* VUregsn) { _vuRegsADD(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDi(_VURegsNum* VUregsn) { _vuRegsADDi(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDq(_VURegsNum* VUregsn) { _vuRegsADDq(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDx(_VURegsNum* VUregsn) { _vuRegsADDx(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDy(_VURegsNum* VUregsn) { _vuRegsADDy(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDz(_VURegsNum* VUregsn) { _vuRegsADDz(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDw(_VURegsNum* VUregsn) { _vuRegsADDw(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDA(_VURegsNum* VUregsn) { _vuRegsADDA(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAi(_VURegsNum* VUregsn) { _vuRegsADDAi(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAq(_VURegsNum* VUregsn) { _vuRegsADDAq(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAx(_VURegsNum* VUregsn) { _vuRegsADDAx(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAy(_VURegsNum* VUregsn) { _vuRegsADDAy(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAz(_VURegsNum* VUregsn) { _vuRegsADDAz(&vuRegs[0], VUregsn); }
static void VU0regsMI_ADDAw(_VURegsNum* VUregsn) { _vuRegsADDAw(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUB(_VURegsNum* VUregsn) { _vuRegsSUB(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBi(_VURegsNum* VUregsn) { _vuRegsSUBi(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBq(_VURegsNum* VUregsn) { _vuRegsSUBq(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBx(_VURegsNum* VUregsn) { _vuRegsSUBx(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBy(_VURegsNum* VUregsn) { _vuRegsSUBy(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBz(_VURegsNum* VUregsn) { _vuRegsSUBz(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBw(_VURegsNum* VUregsn) { _vuRegsSUBw(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBA(_VURegsNum* VUregsn) { _vuRegsSUBA(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAi(_VURegsNum* VUregsn) { _vuRegsSUBAi(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAq(_VURegsNum* VUregsn) { _vuRegsSUBAq(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAx(_VURegsNum* VUregsn) { _vuRegsSUBAx(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAy(_VURegsNum* VUregsn) { _vuRegsSUBAy(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAz(_VURegsNum* VUregsn) { _vuRegsSUBAz(&vuRegs[0], VUregsn); }
static void VU0regsMI_SUBAw(_VURegsNum* VUregsn) { _vuRegsSUBAw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MUL(_VURegsNum* VUregsn) { _vuRegsMUL(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULi(_VURegsNum* VUregsn) { _vuRegsMULi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULq(_VURegsNum* VUregsn) { _vuRegsMULq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULx(_VURegsNum* VUregsn) { _vuRegsMULx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULy(_VURegsNum* VUregsn) { _vuRegsMULy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULz(_VURegsNum* VUregsn) { _vuRegsMULz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULw(_VURegsNum* VUregsn) { _vuRegsMULw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULA(_VURegsNum* VUregsn) { _vuRegsMULA(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAi(_VURegsNum* VUregsn) { _vuRegsMULAi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAq(_VURegsNum* VUregsn) { _vuRegsMULAq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAx(_VURegsNum* VUregsn) { _vuRegsMULAx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAy(_VURegsNum* VUregsn) { _vuRegsMULAy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAz(_VURegsNum* VUregsn) { _vuRegsMULAz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MULAw(_VURegsNum* VUregsn) { _vuRegsMULAw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADD(_VURegsNum* VUregsn) { _vuRegsMADD(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDi(_VURegsNum* VUregsn) { _vuRegsMADDi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDq(_VURegsNum* VUregsn) { _vuRegsMADDq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDx(_VURegsNum* VUregsn) { _vuRegsMADDx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDy(_VURegsNum* VUregsn) { _vuRegsMADDy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDz(_VURegsNum* VUregsn) { _vuRegsMADDz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDw(_VURegsNum* VUregsn) { _vuRegsMADDw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDA(_VURegsNum* VUregsn) { _vuRegsMADDA(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAi(_VURegsNum* VUregsn) { _vuRegsMADDAi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAq(_VURegsNum* VUregsn) { _vuRegsMADDAq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAx(_VURegsNum* VUregsn) { _vuRegsMADDAx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAy(_VURegsNum* VUregsn) { _vuRegsMADDAy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAz(_VURegsNum* VUregsn) { _vuRegsMADDAz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MADDAw(_VURegsNum* VUregsn) { _vuRegsMADDAw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUB(_VURegsNum* VUregsn) { _vuRegsMSUB(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBi(_VURegsNum* VUregsn) { _vuRegsMSUBi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBq(_VURegsNum* VUregsn) { _vuRegsMSUBq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBx(_VURegsNum* VUregsn) { _vuRegsMSUBx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBy(_VURegsNum* VUregsn) { _vuRegsMSUBy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBz(_VURegsNum* VUregsn) { _vuRegsMSUBz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBw(_VURegsNum* VUregsn) { _vuRegsMSUBw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBA(_VURegsNum* VUregsn) { _vuRegsMSUBA(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAi(_VURegsNum* VUregsn) { _vuRegsMSUBAi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAq(_VURegsNum* VUregsn) { _vuRegsMSUBAq(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAx(_VURegsNum* VUregsn) { _vuRegsMSUBAx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAy(_VURegsNum* VUregsn) { _vuRegsMSUBAy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAz(_VURegsNum* VUregsn) { _vuRegsMSUBAz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MSUBAw(_VURegsNum* VUregsn) { _vuRegsMSUBAw(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAX(_VURegsNum* VUregsn) { _vuRegsMAX(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAXi(_VURegsNum* VUregsn) { _vuRegsMAXi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAXx(_VURegsNum* VUregsn) { _vuRegsMAXx_(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAXy(_VURegsNum* VUregsn) { _vuRegsMAXy_(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAXz(_VURegsNum* VUregsn) { _vuRegsMAXz_(&vuRegs[0], VUregsn); }
static void VU0regsMI_MAXw(_VURegsNum* VUregsn) { _vuRegsMAXw_(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINI(_VURegsNum* VUregsn) { _vuRegsMINI(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINIi(_VURegsNum* VUregsn) { _vuRegsMINIi(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINIx(_VURegsNum* VUregsn) { _vuRegsMINIx(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINIy(_VURegsNum* VUregsn) { _vuRegsMINIy(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINIz(_VURegsNum* VUregsn) { _vuRegsMINIz(&vuRegs[0], VUregsn); }
static void VU0regsMI_MINIw(_VURegsNum* VUregsn) { _vuRegsMINIw(&vuRegs[0], VUregsn); }
static void VU0regsMI_OPMULA(_VURegsNum* VUregsn) { _vuRegsOPMULA(&vuRegs[0], VUregsn); }
static void VU0regsMI_OPMSUB(_VURegsNum* VUregsn) { _vuRegsOPMSUB(&vuRegs[0], VUregsn); }
static void VU0regsMI_NOP(_VURegsNum* VUregsn) { _vuRegsNOP(&vuRegs[0], VUregsn); }
static void VU0regsMI_FTOI0(_VURegsNum* VUregsn) { _vuRegsFTOI0(&vuRegs[0], VUregsn); }
static void VU0regsMI_FTOI4(_VURegsNum* VUregsn) { _vuRegsFTOI4(&vuRegs[0], VUregsn); }
static void VU0regsMI_FTOI12(_VURegsNum* VUregsn) { _vuRegsFTOI12(&vuRegs[0], VUregsn); }
static void VU0regsMI_FTOI15(_VURegsNum* VUregsn) { _vuRegsFTOI15(&vuRegs[0], VUregsn); }
static void VU0regsMI_ITOF0(_VURegsNum* VUregsn) { _vuRegsITOF0(&vuRegs[0], VUregsn); }
static void VU0regsMI_ITOF4(_VURegsNum* VUregsn) { _vuRegsITOF4(&vuRegs[0], VUregsn); }
static void VU0regsMI_ITOF12(_VURegsNum* VUregsn) { _vuRegsITOF12(&vuRegs[0], VUregsn); }
static void VU0regsMI_ITOF15(_VURegsNum* VUregsn) { _vuRegsITOF15(&vuRegs[0], VUregsn); }
static void VU0regsMI_CLIP(_VURegsNum* VUregsn) { _vuRegsCLIP(&vuRegs[0], VUregsn); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU0regsMI_DIV(_VURegsNum* VUregsn) { _vuRegsDIV(&vuRegs[0], VUregsn); }
static void VU0regsMI_SQRT(_VURegsNum* VUregsn) { _vuRegsSQRT(&vuRegs[0], VUregsn); }
static void VU0regsMI_RSQRT(_VURegsNum* VUregsn) { _vuRegsRSQRT(&vuRegs[0], VUregsn); }
static void VU0regsMI_IADD(_VURegsNum* VUregsn) { _vuRegsIADD(&vuRegs[0], VUregsn); }
static void VU0regsMI_IADDI(_VURegsNum* VUregsn) { _vuRegsIADDI(&vuRegs[0], VUregsn); }
static void VU0regsMI_IADDIU(_VURegsNum* VUregsn) { _vuRegsIADDIU(&vuRegs[0], VUregsn); }
static void VU0regsMI_IAND(_VURegsNum* VUregsn) { _vuRegsIAND(&vuRegs[0], VUregsn); }
static void VU0regsMI_IOR(_VURegsNum* VUregsn) { _vuRegsIOR(&vuRegs[0], VUregsn); }
static void VU0regsMI_ISUB(_VURegsNum* VUregsn) { _vuRegsISUB(&vuRegs[0], VUregsn); }
static void VU0regsMI_ISUBIU(_VURegsNum* VUregsn) { _vuRegsISUBIU(&vuRegs[0], VUregsn); }
static void VU0regsMI_MOVE(_VURegsNum* VUregsn) { _vuRegsMOVE(&vuRegs[0], VUregsn); }
static void VU0regsMI_MFIR(_VURegsNum* VUregsn) { _vuRegsMFIR(&vuRegs[0], VUregsn); }
static void VU0regsMI_MTIR(_VURegsNum* VUregsn) { _vuRegsMTIR(&vuRegs[0], VUregsn); }
static void VU0regsMI_MR32(_VURegsNum* VUregsn) { _vuRegsMR32(&vuRegs[0], VUregsn); }
static void VU0regsMI_LQ(_VURegsNum* VUregsn) { _vuRegsLQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_LQD(_VURegsNum* VUregsn) { _vuRegsLQD(&vuRegs[0], VUregsn); }
static void VU0regsMI_LQI(_VURegsNum* VUregsn) { _vuRegsLQI(&vuRegs[0], VUregsn); }
static void VU0regsMI_SQ(_VURegsNum* VUregsn) { _vuRegsSQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_SQD(_VURegsNum* VUregsn) { _vuRegsSQD(&vuRegs[0], VUregsn); }
static void VU0regsMI_SQI(_VURegsNum* VUregsn) { _vuRegsSQI(&vuRegs[0], VUregsn); }
static void VU0regsMI_ILW(_VURegsNum* VUregsn) { _vuRegsILW(&vuRegs[0], VUregsn); }
static void VU0regsMI_ISW(_VURegsNum* VUregsn) { _vuRegsISW(&vuRegs[0], VUregsn); }
static void VU0regsMI_ILWR(_VURegsNum* VUregsn) { _vuRegsILWR(&vuRegs[0], VUregsn); }
static void VU0regsMI_ISWR(_VURegsNum* VUregsn) { _vuRegsISWR(&vuRegs[0], VUregsn); }
static void VU0regsMI_RINIT(_VURegsNum* VUregsn) { _vuRegsRINIT(&vuRegs[0], VUregsn); }
static void VU0regsMI_RGET(_VURegsNum* VUregsn) { _vuRegsRGET(&vuRegs[0], VUregsn); }
static void VU0regsMI_RNEXT(_VURegsNum* VUregsn) { _vuRegsRNEXT(&vuRegs[0], VUregsn); }
static void VU0regsMI_RXOR(_VURegsNum* VUregsn) { _vuRegsRXOR(&vuRegs[0], VUregsn); }
static void VU0regsMI_WAITQ(_VURegsNum* VUregsn) { _vuRegsWAITQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_FSAND(_VURegsNum* VUregsn) { _vuRegsFSAND(&vuRegs[0], VUregsn); }
static void VU0regsMI_FSEQ(_VURegsNum* VUregsn) { _vuRegsFSEQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_FSOR(_VURegsNum* VUregsn) { _vuRegsFSOR(&vuRegs[0], VUregsn); }
static void VU0regsMI_FSSET(_VURegsNum* VUregsn) { _vuRegsFSSET(&vuRegs[0], VUregsn); }
static void VU0regsMI_FMAND(_VURegsNum* VUregsn) { _vuRegsFMAND(&vuRegs[0], VUregsn); }
static void VU0regsMI_FMEQ(_VURegsNum* VUregsn) { _vuRegsFMEQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_FMOR(_VURegsNum* VUregsn) { _vuRegsFMOR(&vuRegs[0], VUregsn); }
static void VU0regsMI_FCAND(_VURegsNum* VUregsn) { _vuRegsFCAND(&vuRegs[0], VUregsn); }
static void VU0regsMI_FCEQ(_VURegsNum* VUregsn) { _vuRegsFCEQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_FCOR(_VURegsNum* VUregsn) { _vuRegsFCOR(&vuRegs[0], VUregsn); }
static void VU0regsMI_FCSET(_VURegsNum* VUregsn) { _vuRegsFCSET(&vuRegs[0], VUregsn); }
static void VU0regsMI_FCGET(_VURegsNum* VUregsn) { _vuRegsFCGET(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBEQ(_VURegsNum* VUregsn) { _vuRegsIBEQ(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBGEZ(_VURegsNum* VUregsn) { _vuRegsIBGEZ(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBGTZ(_VURegsNum* VUregsn) { _vuRegsIBGTZ(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBLTZ(_VURegsNum* VUregsn) { _vuRegsIBLTZ(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBLEZ(_VURegsNum* VUregsn) { _vuRegsIBLEZ(&vuRegs[0], VUregsn); }
static void VU0regsMI_IBNE(_VURegsNum* VUregsn) { _vuRegsIBNE(&vuRegs[0], VUregsn); }
static void VU0regsMI_B(_VURegsNum* VUregsn) { _vuRegsB(&vuRegs[0], VUregsn); }
static void VU0regsMI_BAL(_VURegsNum* VUregsn) { _vuRegsBAL(&vuRegs[0], VUregsn); }
static void VU0regsMI_JR(_VURegsNum* VUregsn) { _vuRegsJR(&vuRegs[0], VUregsn); }
static void VU0regsMI_JALR(_VURegsNum* VUregsn) { _vuRegsJALR(&vuRegs[0], VUregsn); }
static void VU0regsMI_MFP(_VURegsNum* VUregsn) { _vuRegsMFP(&vuRegs[0], VUregsn); }
static void VU0regsMI_WAITP(_VURegsNum* VUregsn) { _vuRegsWAITP(&vuRegs[0], VUregsn); }
static void VU0regsMI_ESADD(_VURegsNum* VUregsn) { _vuRegsESADD(&vuRegs[0], VUregsn); }
static void VU0regsMI_ERSADD(_VURegsNum* VUregsn) { _vuRegsERSADD(&vuRegs[0], VUregsn); }
static void VU0regsMI_ELENG(_VURegsNum* VUregsn) { _vuRegsELENG(&vuRegs[0], VUregsn); }
static void VU0regsMI_ERLENG(_VURegsNum* VUregsn) { _vuRegsERLENG(&vuRegs[0], VUregsn); }
static void VU0regsMI_EATANxy(_VURegsNum* VUregsn) { _vuRegsEATANxy(&vuRegs[0], VUregsn); }
static void VU0regsMI_EATANxz(_VURegsNum* VUregsn) { _vuRegsEATANxz(&vuRegs[0], VUregsn); }
static void VU0regsMI_ESUM(_VURegsNum* VUregsn) { _vuRegsESUM(&vuRegs[0], VUregsn); }
static void VU0regsMI_ERCPR(_VURegsNum* VUregsn) { _vuRegsERCPR(&vuRegs[0], VUregsn); }
static void VU0regsMI_ESQRT(_VURegsNum* VUregsn) { _vuRegsESQRT(&vuRegs[0], VUregsn); }
static void VU0regsMI_ERSQRT(_VURegsNum* VUregsn) { _vuRegsERSQRT(&vuRegs[0], VUregsn); }
static void VU0regsMI_ESIN(_VURegsNum* VUregsn) { _vuRegsESIN(&vuRegs[0], VUregsn); }
static void VU0regsMI_EATAN(_VURegsNum* VUregsn) { _vuRegsEATAN(&vuRegs[0], VUregsn); }
static void VU0regsMI_EEXP(_VURegsNum* VUregsn) { _vuRegsEEXP(&vuRegs[0], VUregsn); }
static void VU0regsMI_XITOP(_VURegsNum* VUregsn) { _vuRegsXITOP(&vuRegs[0], VUregsn); }
static void VU0regsMI_XGKICK(_VURegsNum* VUregsn) { _vuRegsXGKICK(&vuRegs[0], VUregsn); }
static void VU0regsMI_XTOP(_VURegsNum* VUregsn) { _vuRegsXTOP(&vuRegs[0], VUregsn); }

static void VU0unknown(void) { }
static void VU0regsunknown(_VURegsNum* VUregsn) { }

// --------------------------------------------------------------------------------------
//  VU1
// --------------------------------------------------------------------------------------

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU1MI_ABS()  { _vuABS(&vuRegs[1]); }
static void VU1MI_ADD()  { _vuADD(&vuRegs[1]); }
static void VU1MI_ADDi() { _vuADDi(&vuRegs[1]); }
static void VU1MI_ADDq() { _vuADDq(&vuRegs[1]); }
static void VU1MI_ADDx() { _vuADDx(&vuRegs[1]); }
static void VU1MI_ADDy() { _vuADDy(&vuRegs[1]); }
static void VU1MI_ADDz() { _vuADDz(&vuRegs[1]); }
static void VU1MI_ADDw() { _vuADDw(&vuRegs[1]); }
static void VU1MI_ADDA() { _vuADDA(&vuRegs[1]); }
static void VU1MI_ADDAi() { _vuADDAi(&vuRegs[1]); }
static void VU1MI_ADDAq() { _vuADDAq(&vuRegs[1]); }
static void VU1MI_ADDAx() { _vuADDAx(&vuRegs[1]); }
static void VU1MI_ADDAy() { _vuADDAy(&vuRegs[1]); }
static void VU1MI_ADDAz() { _vuADDAz(&vuRegs[1]); }
static void VU1MI_ADDAw() { _vuADDAw(&vuRegs[1]); }
static void VU1MI_SUB()  { _vuSUB(&vuRegs[1]); }
static void VU1MI_SUBi() { _vuSUBi(&vuRegs[1]); }
static void VU1MI_SUBq() { _vuSUBq(&vuRegs[1]); }
static void VU1MI_SUBx() { _vuSUBx(&vuRegs[1]); }
static void VU1MI_SUBy() { _vuSUBy(&vuRegs[1]); }
static void VU1MI_SUBz() { _vuSUBz(&vuRegs[1]); }
static void VU1MI_SUBw() { _vuSUBw(&vuRegs[1]); }
static void VU1MI_SUBA()  { _vuSUBA(&vuRegs[1]); }
static void VU1MI_SUBAi() { _vuSUBAi(&vuRegs[1]); }
static void VU1MI_SUBAq() { _vuSUBAq(&vuRegs[1]); }
static void VU1MI_SUBAx() { _vuSUBAx(&vuRegs[1]); }
static void VU1MI_SUBAy() { _vuSUBAy(&vuRegs[1]); }
static void VU1MI_SUBAz() { _vuSUBAz(&vuRegs[1]); }
static void VU1MI_SUBAw() { _vuSUBAw(&vuRegs[1]); }
static void VU1MI_MUL()  { _vuMUL(&vuRegs[1]); }
static void VU1MI_MULi() { _vuMULi(&vuRegs[1]); }
static void VU1MI_MULq() { _vuMULq(&vuRegs[1]); }
static void VU1MI_MULx() { _vuMULx(&vuRegs[1]); }
static void VU1MI_MULy() { _vuMULy(&vuRegs[1]); }
static void VU1MI_MULz() { _vuMULz(&vuRegs[1]); }
static void VU1MI_MULw() { _vuMULw(&vuRegs[1]); }
static void VU1MI_MULA()  { _vuMULA(&vuRegs[1]); }
static void VU1MI_MULAi() { _vuMULAi(&vuRegs[1]); }
static void VU1MI_MULAq() { _vuMULAq(&vuRegs[1]); }
static void VU1MI_MULAx() { _vuMULAx(&vuRegs[1]); }
static void VU1MI_MULAy() { _vuMULAy(&vuRegs[1]); }
static void VU1MI_MULAz() { _vuMULAz(&vuRegs[1]); }
static void VU1MI_MULAw() { _vuMULAw(&vuRegs[1]); }
static void VU1MI_MADD()  { _vuMADD(&vuRegs[1]); }
static void VU1MI_MADDi() { _vuMADDi(&vuRegs[1]); }
static void VU1MI_MADDq() { _vuMADDq(&vuRegs[1]); }
static void VU1MI_MADDx() { _vuMADDx(&vuRegs[1]); }
static void VU1MI_MADDy() { _vuMADDy(&vuRegs[1]); }
static void VU1MI_MADDz() { _vuMADDz(&vuRegs[1]); }
static void VU1MI_MADDw() { _vuMADDw(&vuRegs[1]); }
static void VU1MI_MADDA()  { _vuMADDA(&vuRegs[1]); }
static void VU1MI_MADDAi() { _vuMADDAi(&vuRegs[1]); }
static void VU1MI_MADDAq() { _vuMADDAq(&vuRegs[1]); }
static void VU1MI_MADDAx() { _vuMADDAx(&vuRegs[1]); }
static void VU1MI_MADDAy() { _vuMADDAy(&vuRegs[1]); }
static void VU1MI_MADDAz() { _vuMADDAz(&vuRegs[1]); }
static void VU1MI_MADDAw() { _vuMADDAw(&vuRegs[1]); }
static void VU1MI_MSUB()  { _vuMSUB(&vuRegs[1]); }
static void VU1MI_MSUBi() { _vuMSUBi(&vuRegs[1]); }
static void VU1MI_MSUBq() { _vuMSUBq(&vuRegs[1]); }
static void VU1MI_MSUBx() { _vuMSUBx(&vuRegs[1]); }
static void VU1MI_MSUBy() { _vuMSUBy(&vuRegs[1]); }
static void VU1MI_MSUBz() { _vuMSUBz(&vuRegs[1]); }
static void VU1MI_MSUBw() { _vuMSUBw(&vuRegs[1]); }
static void VU1MI_MSUBA()  { _vuMSUBA(&vuRegs[1]); }
static void VU1MI_MSUBAi() { _vuMSUBAi(&vuRegs[1]); }
static void VU1MI_MSUBAq() { _vuMSUBAq(&vuRegs[1]); }
static void VU1MI_MSUBAx() { _vuMSUBAx(&vuRegs[1]); }
static void VU1MI_MSUBAy() { _vuMSUBAy(&vuRegs[1]); }
static void VU1MI_MSUBAz() { _vuMSUBAz(&vuRegs[1]); }
static void VU1MI_MSUBAw() { _vuMSUBAw(&vuRegs[1]); }
static void VU1MI_MAX()  { _vuMAX(&vuRegs[1]); }
static void VU1MI_MAXi() { _vuMAXi(&vuRegs[1]); }
static void VU1MI_MAXx() { _vuMAXx(&vuRegs[1]); }
static void VU1MI_MAXy() { _vuMAXy(&vuRegs[1]); }
static void VU1MI_MAXz() { _vuMAXz(&vuRegs[1]); }
static void VU1MI_MAXw() { _vuMAXw(&vuRegs[1]); }
static void VU1MI_MINI()  { _vuMINI(&vuRegs[1]); }
static void VU1MI_MINIi() { _vuMINIi(&vuRegs[1]); }
static void VU1MI_MINIx() { _vuMINIx(&vuRegs[1]); }
static void VU1MI_MINIy() { _vuMINIy(&vuRegs[1]); }
static void VU1MI_MINIz() { _vuMINIz(&vuRegs[1]); }
static void VU1MI_MINIw() { _vuMINIw(&vuRegs[1]); }
static void VU1MI_OPMULA() { _vuOPMULA(&vuRegs[1]); }
static void VU1MI_OPMSUB() { _vuOPMSUB(&vuRegs[1]); }
static void VU1MI_NOP() { _vuNOP(&vuRegs[1]); }
static void VU1MI_FTOI0()  { _vuFTOI0(&vuRegs[1]); }
static void VU1MI_FTOI4()  { _vuFTOI4(&vuRegs[1]); }
static void VU1MI_FTOI12() { _vuFTOI12(&vuRegs[1]); }
static void VU1MI_FTOI15() { _vuFTOI15(&vuRegs[1]); }
static void VU1MI_ITOF0()  { _vuITOF0(&vuRegs[1]); }
static void VU1MI_ITOF4()  { _vuITOF4(&vuRegs[1]); }
static void VU1MI_ITOF12() { _vuITOF12(&vuRegs[1]); }
static void VU1MI_ITOF15() { _vuITOF15(&vuRegs[1]); }
static void VU1MI_CLIP() { _vuCLIP(&vuRegs[1]); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU1MI_DIV() { _vuDIV(&vuRegs[1]); }
static void VU1MI_SQRT() { _vuSQRT(&vuRegs[1]); }
static void VU1MI_RSQRT() { _vuRSQRT(&vuRegs[1]); }
static void VU1MI_IADD() { _vuIADD(&vuRegs[1]); }
static void VU1MI_IADDI() { _vuIADDI(&vuRegs[1]); }
static void VU1MI_IADDIU() { _vuIADDIU(&vuRegs[1]); }
static void VU1MI_IAND() { _vuIAND(&vuRegs[1]); }
static void VU1MI_IOR() { _vuIOR(&vuRegs[1]); }
static void VU1MI_ISUB() { _vuISUB(&vuRegs[1]); }
static void VU1MI_ISUBIU() { _vuISUBIU(&vuRegs[1]); }
static void VU1MI_MOVE() { _vuMOVE(&vuRegs[1]); }
static void VU1MI_MFIR() { _vuMFIR(&vuRegs[1]); }
static void VU1MI_MTIR() { _vuMTIR(&vuRegs[1]); }
static void VU1MI_MR32() { _vuMR32(&vuRegs[1]); }
static void VU1MI_LQ() { _vuLQ(&vuRegs[1]); }
static void VU1MI_LQD() { _vuLQD(&vuRegs[1]); }
static void VU1MI_LQI() { _vuLQI(&vuRegs[1]); }
static void VU1MI_SQ() { _vuSQ(&vuRegs[1]); }
static void VU1MI_SQD() { _vuSQD(&vuRegs[1]); }
static void VU1MI_SQI() { _vuSQI(&vuRegs[1]); }
static void VU1MI_ILW() { _vuILW(&vuRegs[1]); }
static void VU1MI_ISW() { _vuISW(&vuRegs[1]); }
static void VU1MI_ILWR() { _vuILWR(&vuRegs[1]); }
static void VU1MI_ISWR() { _vuISWR(&vuRegs[1]); }
static void VU1MI_RINIT() { vuRegs[1].VI[REG_R].UL = 0x3F800000 | (vuRegs[1].VF[((vuRegs[1].code >> 11) & 0x1F)].UL[((vuRegs[1].code >> 21) & 0x03)] & 0x007FFFFF); }
static void VU1MI_RGET()  { _vuRGET(&vuRegs[1]); }
static void VU1MI_RNEXT() { _vuRNEXT(&vuRegs[1]); }
static void VU1MI_RXOR()  { vuRegs[1].VI[REG_R].UL = 0x3F800000 | ((vuRegs[1].VI[REG_R].UL ^ vuRegs[1].VF[((vuRegs[1].code >> 11) & 0x1F)].UL[((vuRegs[1].code >> 21) & 0x03)]) & 0x007FFFFF); }
static void VU1MI_WAITQ() { }
static void VU1MI_FSAND() { _vuFSAND(&vuRegs[1]); }
static void VU1MI_FSEQ()  { _vuFSEQ(&vuRegs[1]); }
static void VU1MI_FSOR()  { _vuFSOR(&vuRegs[1]); }
static void VU1MI_FSSET() { _vuFSSET(&vuRegs[1]); }
static void VU1MI_FMAND() { _vuFMAND(&vuRegs[1]); }
static void VU1MI_FMEQ()  { _vuFMEQ(&vuRegs[1]); }
static void VU1MI_FMOR()  { _vuFMOR(&vuRegs[1]); }
static void VU1MI_FCAND() { _vuFCAND(&vuRegs[1]); }
static void VU1MI_FCEQ()  { _vuFCEQ(&vuRegs[1]); }
static void VU1MI_FCOR()  { _vuFCOR(&vuRegs[1]); }
static void VU1MI_FCSET() { vuRegs[1].clipflag = (u32)(vuRegs[1].code & 0xFFFFFF); }
static void VU1MI_FCGET() { _vuFCGET(&vuRegs[1]); }
static void VU1MI_IBEQ() { _vuIBEQ(&vuRegs[1]); }
static void VU1MI_IBGEZ() { _vuIBGEZ(&vuRegs[1]); }
static void VU1MI_IBGTZ() { _vuIBGTZ(&vuRegs[1]); }
static void VU1MI_IBLTZ() { _vuIBLTZ(&vuRegs[1]); }
static void VU1MI_IBLEZ() { _vuIBLEZ(&vuRegs[1]); }
static void VU1MI_IBNE() { _vuIBNE(&vuRegs[1]); }
static void VU1MI_B()   { _vuB(&vuRegs[1]); }
static void VU1MI_BAL() { _vuBAL(&vuRegs[1]); }
static void VU1MI_JR()   { _vuJR(&vuRegs[1]); }
static void VU1MI_JALR() { _vuJALR(&vuRegs[1]); }
static void VU1MI_MFP() { _vuMFP(&vuRegs[1]); }
static void VU1MI_WAITP() { }
static void VU1MI_ESADD()   { _vuESADD(&vuRegs[1]); }
static void VU1MI_ERSADD()  { _vuERSADD(&vuRegs[1]); }
static void VU1MI_ELENG()   { _vuELENG(&vuRegs[1]); }
static void VU1MI_ERLENG()  { _vuERLENG(&vuRegs[1]); }
static void VU1MI_EATANxy() { _vuEATANxy(&vuRegs[1]); }
static void VU1MI_EATANxz() { _vuEATANxz(&vuRegs[1]); }
static void VU1MI_ESUM()    { _vuESUM(&vuRegs[1]); }
static void VU1MI_ERCPR()   { _vuERCPR(&vuRegs[1]); }
static void VU1MI_ESQRT()   { _vuESQRT(&vuRegs[1]); }
static void VU1MI_ERSQRT()  { _vuERSQRT(&vuRegs[1]); }
static void VU1MI_ESIN()    { _vuESIN(&vuRegs[1]); }
static void VU1MI_EATAN()   { _vuEATAN(&vuRegs[1]); }
static void VU1MI_EEXP()    { _vuEEXP(&vuRegs[1]); }
static void VU1MI_XITOP()   { _vuXITOP(&vuRegs[1]); }
static void VU1MI_XGKICK()  { _vuXGKICK(&vuRegs[1]); }
static void VU1MI_XTOP()    { _vuXTOP(&vuRegs[1]); }



/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU1regsMI_ABS(_VURegsNum* VUregsn) { _vuRegsABS(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADD(_VURegsNum* VUregsn) { _vuRegsADD(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDi(_VURegsNum* VUregsn) { _vuRegsADDi(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDq(_VURegsNum* VUregsn) { _vuRegsADDq(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDx(_VURegsNum* VUregsn) { _vuRegsADDx(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDy(_VURegsNum* VUregsn) { _vuRegsADDy(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDz(_VURegsNum* VUregsn) { _vuRegsADDz(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDw(_VURegsNum* VUregsn) { _vuRegsADDw(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDA(_VURegsNum* VUregsn) { _vuRegsADDA(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAi(_VURegsNum* VUregsn) { _vuRegsADDAi(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAq(_VURegsNum* VUregsn) { _vuRegsADDAq(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAx(_VURegsNum* VUregsn) { _vuRegsADDAx(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAy(_VURegsNum* VUregsn) { _vuRegsADDAy(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAz(_VURegsNum* VUregsn) { _vuRegsADDAz(&vuRegs[1], VUregsn); }
static void VU1regsMI_ADDAw(_VURegsNum* VUregsn) { _vuRegsADDAw(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUB(_VURegsNum* VUregsn) { _vuRegsSUB(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBi(_VURegsNum* VUregsn) { _vuRegsSUBi(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBq(_VURegsNum* VUregsn) { _vuRegsSUBq(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBx(_VURegsNum* VUregsn) { _vuRegsSUBx(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBy(_VURegsNum* VUregsn) { _vuRegsSUBy(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBz(_VURegsNum* VUregsn) { _vuRegsSUBz(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBw(_VURegsNum* VUregsn) { _vuRegsSUBw(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBA(_VURegsNum* VUregsn) { _vuRegsSUBA(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAi(_VURegsNum* VUregsn) { _vuRegsSUBAi(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAq(_VURegsNum* VUregsn) { _vuRegsSUBAq(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAx(_VURegsNum* VUregsn) { _vuRegsSUBAx(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAy(_VURegsNum* VUregsn) { _vuRegsSUBAy(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAz(_VURegsNum* VUregsn) { _vuRegsSUBAz(&vuRegs[1], VUregsn); }
static void VU1regsMI_SUBAw(_VURegsNum* VUregsn) { _vuRegsSUBAw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MUL(_VURegsNum* VUregsn) { _vuRegsMUL(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULi(_VURegsNum* VUregsn) { _vuRegsMULi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULq(_VURegsNum* VUregsn) { _vuRegsMULq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULx(_VURegsNum* VUregsn) { _vuRegsMULx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULy(_VURegsNum* VUregsn) { _vuRegsMULy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULz(_VURegsNum* VUregsn) { _vuRegsMULz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULw(_VURegsNum* VUregsn) { _vuRegsMULw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULA(_VURegsNum* VUregsn) { _vuRegsMULA(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAi(_VURegsNum* VUregsn) { _vuRegsMULAi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAq(_VURegsNum* VUregsn) { _vuRegsMULAq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAx(_VURegsNum* VUregsn) { _vuRegsMULAx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAy(_VURegsNum* VUregsn) { _vuRegsMULAy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAz(_VURegsNum* VUregsn) { _vuRegsMULAz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MULAw(_VURegsNum* VUregsn) { _vuRegsMULAw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADD(_VURegsNum* VUregsn) { _vuRegsMADD(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDi(_VURegsNum* VUregsn) { _vuRegsMADDi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDq(_VURegsNum* VUregsn) { _vuRegsMADDq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDx(_VURegsNum* VUregsn) { _vuRegsMADDx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDy(_VURegsNum* VUregsn) { _vuRegsMADDy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDz(_VURegsNum* VUregsn) { _vuRegsMADDz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDw(_VURegsNum* VUregsn) { _vuRegsMADDw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDA(_VURegsNum* VUregsn) { _vuRegsMADDA(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAi(_VURegsNum* VUregsn) { _vuRegsMADDAi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAq(_VURegsNum* VUregsn) { _vuRegsMADDAq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAx(_VURegsNum* VUregsn) { _vuRegsMADDAx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAy(_VURegsNum* VUregsn) { _vuRegsMADDAy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAz(_VURegsNum* VUregsn) { _vuRegsMADDAz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MADDAw(_VURegsNum* VUregsn) { _vuRegsMADDAw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUB(_VURegsNum* VUregsn) { _vuRegsMSUB(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBi(_VURegsNum* VUregsn) { _vuRegsMSUBi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBq(_VURegsNum* VUregsn) { _vuRegsMSUBq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBx(_VURegsNum* VUregsn) { _vuRegsMSUBx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBy(_VURegsNum* VUregsn) { _vuRegsMSUBy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBz(_VURegsNum* VUregsn) { _vuRegsMSUBz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBw(_VURegsNum* VUregsn) { _vuRegsMSUBw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBA(_VURegsNum* VUregsn) { _vuRegsMSUBA(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAi(_VURegsNum* VUregsn) { _vuRegsMSUBAi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAq(_VURegsNum* VUregsn) { _vuRegsMSUBAq(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAx(_VURegsNum* VUregsn) { _vuRegsMSUBAx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAy(_VURegsNum* VUregsn) { _vuRegsMSUBAy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAz(_VURegsNum* VUregsn) { _vuRegsMSUBAz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MSUBAw(_VURegsNum* VUregsn) { _vuRegsMSUBAw(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAX(_VURegsNum* VUregsn) { _vuRegsMAX(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAXi(_VURegsNum* VUregsn) { _vuRegsMAXi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAXx(_VURegsNum* VUregsn) { _vuRegsMAXx_(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAXy(_VURegsNum* VUregsn) { _vuRegsMAXy_(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAXz(_VURegsNum* VUregsn) { _vuRegsMAXz_(&vuRegs[1], VUregsn); }
static void VU1regsMI_MAXw(_VURegsNum* VUregsn) { _vuRegsMAXw_(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINI(_VURegsNum* VUregsn) { _vuRegsMINI(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINIi(_VURegsNum* VUregsn) { _vuRegsMINIi(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINIx(_VURegsNum* VUregsn) { _vuRegsMINIx(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINIy(_VURegsNum* VUregsn) { _vuRegsMINIy(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINIz(_VURegsNum* VUregsn) { _vuRegsMINIz(&vuRegs[1], VUregsn); }
static void VU1regsMI_MINIw(_VURegsNum* VUregsn) { _vuRegsMINIw(&vuRegs[1], VUregsn); }
static void VU1regsMI_OPMULA(_VURegsNum* VUregsn) { _vuRegsOPMULA(&vuRegs[1], VUregsn); }
static void VU1regsMI_OPMSUB(_VURegsNum* VUregsn) { _vuRegsOPMSUB(&vuRegs[1], VUregsn); }
static void VU1regsMI_NOP(_VURegsNum* VUregsn) { _vuRegsNOP(&vuRegs[1], VUregsn); }
static void VU1regsMI_FTOI0(_VURegsNum* VUregsn) { _vuRegsFTOI0(&vuRegs[1], VUregsn); }
static void VU1regsMI_FTOI4(_VURegsNum* VUregsn) { _vuRegsFTOI4(&vuRegs[1], VUregsn); }
static void VU1regsMI_FTOI12(_VURegsNum* VUregsn) { _vuRegsFTOI12(&vuRegs[1], VUregsn); }
static void VU1regsMI_FTOI15(_VURegsNum* VUregsn) { _vuRegsFTOI15(&vuRegs[1], VUregsn); }
static void VU1regsMI_ITOF0(_VURegsNum* VUregsn) { _vuRegsITOF0(&vuRegs[1], VUregsn); }
static void VU1regsMI_ITOF4(_VURegsNum* VUregsn) { _vuRegsITOF4(&vuRegs[1], VUregsn); }
static void VU1regsMI_ITOF12(_VURegsNum* VUregsn) { _vuRegsITOF12(&vuRegs[1], VUregsn); }
static void VU1regsMI_ITOF15(_VURegsNum* VUregsn) { _vuRegsITOF15(&vuRegs[1], VUregsn); }
static void VU1regsMI_CLIP(_VURegsNum* VUregsn) { _vuRegsCLIP(&vuRegs[1], VUregsn); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU1regsMI_DIV(_VURegsNum* VUregsn) { _vuRegsDIV(&vuRegs[1], VUregsn); }
static void VU1regsMI_SQRT(_VURegsNum* VUregsn) { _vuRegsSQRT(&vuRegs[1], VUregsn); }
static void VU1regsMI_RSQRT(_VURegsNum* VUregsn) { _vuRegsRSQRT(&vuRegs[1], VUregsn); }
static void VU1regsMI_IADD(_VURegsNum* VUregsn) { _vuRegsIADD(&vuRegs[1], VUregsn); }
static void VU1regsMI_IADDI(_VURegsNum* VUregsn) { _vuRegsIADDI(&vuRegs[1], VUregsn); }
static void VU1regsMI_IADDIU(_VURegsNum* VUregsn) { _vuRegsIADDIU(&vuRegs[1], VUregsn); }
static void VU1regsMI_IAND(_VURegsNum* VUregsn) { _vuRegsIAND(&vuRegs[1], VUregsn); }
static void VU1regsMI_IOR(_VURegsNum* VUregsn) { _vuRegsIOR(&vuRegs[1], VUregsn); }
static void VU1regsMI_ISUB(_VURegsNum* VUregsn) { _vuRegsISUB(&vuRegs[1], VUregsn); }
static void VU1regsMI_ISUBIU(_VURegsNum* VUregsn) { _vuRegsISUBIU(&vuRegs[1], VUregsn); }
static void VU1regsMI_MOVE(_VURegsNum* VUregsn) { _vuRegsMOVE(&vuRegs[1], VUregsn); }
static void VU1regsMI_MFIR(_VURegsNum* VUregsn) { _vuRegsMFIR(&vuRegs[1], VUregsn); }
static void VU1regsMI_MTIR(_VURegsNum* VUregsn) { _vuRegsMTIR(&vuRegs[1], VUregsn); }
static void VU1regsMI_MR32(_VURegsNum* VUregsn) { _vuRegsMR32(&vuRegs[1], VUregsn); }
static void VU1regsMI_LQ(_VURegsNum* VUregsn) { _vuRegsLQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_LQD(_VURegsNum* VUregsn) { _vuRegsLQD(&vuRegs[1], VUregsn); }
static void VU1regsMI_LQI(_VURegsNum* VUregsn) { _vuRegsLQI(&vuRegs[1], VUregsn); }
static void VU1regsMI_SQ(_VURegsNum* VUregsn) { _vuRegsSQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_SQD(_VURegsNum* VUregsn) { _vuRegsSQD(&vuRegs[1], VUregsn); }
static void VU1regsMI_SQI(_VURegsNum* VUregsn) { _vuRegsSQI(&vuRegs[1], VUregsn); }
static void VU1regsMI_ILW(_VURegsNum* VUregsn) { _vuRegsILW(&vuRegs[1], VUregsn); }
static void VU1regsMI_ISW(_VURegsNum* VUregsn) { _vuRegsISW(&vuRegs[1], VUregsn); }
static void VU1regsMI_ILWR(_VURegsNum* VUregsn) { _vuRegsILWR(&vuRegs[1], VUregsn); }
static void VU1regsMI_ISWR(_VURegsNum* VUregsn) { _vuRegsISWR(&vuRegs[1], VUregsn); }
static void VU1regsMI_RINIT(_VURegsNum* VUregsn) { _vuRegsRINIT(&vuRegs[1], VUregsn); }
static void VU1regsMI_RGET(_VURegsNum* VUregsn) { _vuRegsRGET(&vuRegs[1], VUregsn); }
static void VU1regsMI_RNEXT(_VURegsNum* VUregsn) { _vuRegsRNEXT(&vuRegs[1], VUregsn); }
static void VU1regsMI_RXOR(_VURegsNum* VUregsn) { _vuRegsRXOR(&vuRegs[1], VUregsn); }
static void VU1regsMI_WAITQ(_VURegsNum* VUregsn) { _vuRegsWAITQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_FSAND(_VURegsNum* VUregsn) { _vuRegsFSAND(&vuRegs[1], VUregsn); }
static void VU1regsMI_FSEQ(_VURegsNum* VUregsn) { _vuRegsFSEQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_FSOR(_VURegsNum* VUregsn) { _vuRegsFSOR(&vuRegs[1], VUregsn); }
static void VU1regsMI_FSSET(_VURegsNum* VUregsn) { _vuRegsFSSET(&vuRegs[1], VUregsn); }
static void VU1regsMI_FMAND(_VURegsNum* VUregsn) { _vuRegsFMAND(&vuRegs[1], VUregsn); }
static void VU1regsMI_FMEQ(_VURegsNum* VUregsn) { _vuRegsFMEQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_FMOR(_VURegsNum* VUregsn) { _vuRegsFMOR(&vuRegs[1], VUregsn); }
static void VU1regsMI_FCAND(_VURegsNum* VUregsn) { _vuRegsFCAND(&vuRegs[1], VUregsn); }
static void VU1regsMI_FCEQ(_VURegsNum* VUregsn) { _vuRegsFCEQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_FCOR(_VURegsNum* VUregsn) { _vuRegsFCOR(&vuRegs[1], VUregsn); }
static void VU1regsMI_FCSET(_VURegsNum* VUregsn) { _vuRegsFCSET(&vuRegs[1], VUregsn); }
static void VU1regsMI_FCGET(_VURegsNum* VUregsn) { _vuRegsFCGET(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBEQ(_VURegsNum* VUregsn) { _vuRegsIBEQ(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBGEZ(_VURegsNum* VUregsn) { _vuRegsIBGEZ(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBGTZ(_VURegsNum* VUregsn) { _vuRegsIBGTZ(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBLTZ(_VURegsNum* VUregsn) { _vuRegsIBLTZ(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBLEZ(_VURegsNum* VUregsn) { _vuRegsIBLEZ(&vuRegs[1], VUregsn); }
static void VU1regsMI_IBNE(_VURegsNum* VUregsn) { _vuRegsIBNE(&vuRegs[1], VUregsn); }
static void VU1regsMI_B(_VURegsNum* VUregsn) { _vuRegsB(&vuRegs[1], VUregsn); }
static void VU1regsMI_BAL(_VURegsNum* VUregsn) { _vuRegsBAL(&vuRegs[1], VUregsn); }
static void VU1regsMI_JR(_VURegsNum* VUregsn) { _vuRegsJR(&vuRegs[1], VUregsn); }
static void VU1regsMI_JALR(_VURegsNum* VUregsn) { _vuRegsJALR(&vuRegs[1], VUregsn); }
static void VU1regsMI_MFP(_VURegsNum* VUregsn) { _vuRegsMFP(&vuRegs[1], VUregsn); }
static void VU1regsMI_WAITP(_VURegsNum* VUregsn) { _vuRegsWAITP(&vuRegs[1], VUregsn); }
static void VU1regsMI_ESADD(_VURegsNum* VUregsn) { _vuRegsESADD(&vuRegs[1], VUregsn); }
static void VU1regsMI_ERSADD(_VURegsNum* VUregsn) { _vuRegsERSADD(&vuRegs[1], VUregsn); }
static void VU1regsMI_ELENG(_VURegsNum* VUregsn) { _vuRegsELENG(&vuRegs[1], VUregsn); }
static void VU1regsMI_ERLENG(_VURegsNum* VUregsn) { _vuRegsERLENG(&vuRegs[1], VUregsn); }
static void VU1regsMI_EATANxy(_VURegsNum* VUregsn) { _vuRegsEATANxy(&vuRegs[1], VUregsn); }
static void VU1regsMI_EATANxz(_VURegsNum* VUregsn) { _vuRegsEATANxz(&vuRegs[1], VUregsn); }
static void VU1regsMI_ESUM(_VURegsNum* VUregsn) { _vuRegsESUM(&vuRegs[1], VUregsn); }
static void VU1regsMI_ERCPR(_VURegsNum* VUregsn) { _vuRegsERCPR(&vuRegs[1], VUregsn); }
static void VU1regsMI_ESQRT(_VURegsNum* VUregsn) { _vuRegsESQRT(&vuRegs[1], VUregsn); }
static void VU1regsMI_ERSQRT(_VURegsNum* VUregsn) { _vuRegsERSQRT(&vuRegs[1], VUregsn); }
static void VU1regsMI_ESIN(_VURegsNum* VUregsn) { _vuRegsESIN(&vuRegs[1], VUregsn); }
static void VU1regsMI_EATAN(_VURegsNum* VUregsn) { _vuRegsEATAN(&vuRegs[1], VUregsn); }
static void VU1regsMI_EEXP(_VURegsNum* VUregsn) { _vuRegsEEXP(&vuRegs[1], VUregsn); }
static void VU1regsMI_XITOP(_VURegsNum* VUregsn) { _vuRegsXITOP(&vuRegs[1], VUregsn); }
static void VU1regsMI_XGKICK(_VURegsNum* VUregsn) { _vuRegsXGKICK(&vuRegs[1], VUregsn); }
static void VU1regsMI_XTOP(_VURegsNum* VUregsn) { _vuRegsXTOP(&vuRegs[1], VUregsn); }

static void VU1unknown(void) { }
static void VU1regsunknown(_VURegsNum* VUregsn) { }

// --------------------------------------------------------------------------------------
//  VU Micromode Tables/Opcodes defs macros
// --------------------------------------------------------------------------------------

#define _vuTablesMess(PREFIX, FNTYPE) \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_00_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MOVE  , PREFIX##MI_LQI   , PREFIX##MI_DIV  , PREFIX##MI_MTIR,  \
	PREFIX##MI_RNEXT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_MFP   , PREFIX##MI_XTOP , PREFIX##MI_XGKICK,  \
	PREFIX##MI_ESADD , PREFIX##MI_EATANxy, PREFIX##MI_ESQRT, PREFIX##MI_ESIN,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_01_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MR32  , PREFIX##MI_SQI   , PREFIX##MI_SQRT , PREFIX##MI_MFIR,  \
	PREFIX##MI_RGET  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##MI_XITOP, PREFIX##unknown,  \
	PREFIX##MI_ERSADD, PREFIX##MI_EATANxz, PREFIX##MI_ERSQRT, PREFIX##MI_EATAN, \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_10_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_LQD   , PREFIX##MI_RSQRT, PREFIX##MI_ILWR,  \
	PREFIX##MI_RINIT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ELENG , PREFIX##MI_ESUM  , PREFIX##MI_ERCPR, PREFIX##MI_EEXP,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_11_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_SQD   , PREFIX##MI_WAITQ, PREFIX##MI_ISWR,  \
	PREFIX##MI_RXOR  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ERLENG, PREFIX##unknown  , PREFIX##MI_WAITP, PREFIX##unknown,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_OPCODE[64] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADD  , PREFIX##MI_ISUB  , PREFIX##MI_IADDI, PREFIX##unknown, /* 0x30 */ \
	PREFIX##MI_IAND  , PREFIX##MI_IOR   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP_T3_00, PREFIX##LowerOP_T3_01, PREFIX##LowerOP_T3_10, PREFIX##LowerOP_T3_11,  \
}; \
 \
alignas(16) const FNTYPE PREFIX##_LOWER_OPCODE[128] = { \
	PREFIX##MI_LQ    , PREFIX##MI_SQ    , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ILW   , PREFIX##MI_ISW   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADDIU, PREFIX##MI_ISUBIU, PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_FCEQ  , PREFIX##MI_FCSET , PREFIX##MI_FCAND, PREFIX##MI_FCOR, /* 0x10 */ \
	PREFIX##MI_FSEQ  , PREFIX##MI_FSSET , PREFIX##MI_FSAND, PREFIX##MI_FSOR, \
	PREFIX##MI_FMEQ  , PREFIX##unknown  , PREFIX##MI_FMAND, PREFIX##MI_FMOR, \
	PREFIX##MI_FCGET , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_B     , PREFIX##MI_BAL   , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##MI_JR    , PREFIX##MI_JALR  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBEQ  , PREFIX##MI_IBNE  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBLTZ , PREFIX##MI_IBGTZ , PREFIX##MI_IBLEZ, PREFIX##MI_IBGEZ, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x40*/  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x50 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x60 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x70 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_00_TABLE[32] = { \
	PREFIX##MI_ADDAx, PREFIX##MI_SUBAx , PREFIX##MI_MADDAx, PREFIX##MI_MSUBAx, \
	PREFIX##MI_ITOF0, PREFIX##MI_FTOI0, PREFIX##MI_MULAx , PREFIX##MI_MULAq , \
	PREFIX##MI_ADDAq, PREFIX##MI_SUBAq, PREFIX##MI_ADDA  , PREFIX##MI_SUBA  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_01_TABLE[32] = { \
	PREFIX##MI_ADDAy , PREFIX##MI_SUBAy  , PREFIX##MI_MADDAy, PREFIX##MI_MSUBAy, \
	PREFIX##MI_ITOF4 , PREFIX##MI_FTOI4 , PREFIX##MI_MULAy , PREFIX##MI_ABS   , \
	PREFIX##MI_MADDAq, PREFIX##MI_MSUBAq, PREFIX##MI_MADDA , PREFIX##MI_MSUBA , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_10_TABLE[32] = { \
	PREFIX##MI_ADDAz , PREFIX##MI_SUBAz  , PREFIX##MI_MADDAz, PREFIX##MI_MSUBAz, \
	PREFIX##MI_ITOF12, PREFIX##MI_FTOI12, PREFIX##MI_MULAz , PREFIX##MI_MULAi , \
	PREFIX##MI_ADDAi, PREFIX##MI_SUBAi , PREFIX##MI_MULA  , PREFIX##MI_OPMULA, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_11_TABLE[32] = { \
	PREFIX##MI_ADDAw , PREFIX##MI_SUBAw  , PREFIX##MI_MADDAw, PREFIX##MI_MSUBAw, \
	PREFIX##MI_ITOF15, PREFIX##MI_FTOI15, PREFIX##MI_MULAw , PREFIX##MI_CLIP  , \
	PREFIX##MI_MADDAi, PREFIX##MI_MSUBAi, PREFIX##unknown  , PREFIX##MI_NOP   , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) const FNTYPE PREFIX##_UPPER_OPCODE[64] = { \
	PREFIX##MI_ADDx  , PREFIX##MI_ADDy  , PREFIX##MI_ADDz  , PREFIX##MI_ADDw, \
	PREFIX##MI_SUBx  , PREFIX##MI_SUBy  , PREFIX##MI_SUBz  , PREFIX##MI_SUBw, \
	PREFIX##MI_MADDx , PREFIX##MI_MADDy , PREFIX##MI_MADDz , PREFIX##MI_MADDw, \
	PREFIX##MI_MSUBx , PREFIX##MI_MSUBy , PREFIX##MI_MSUBz , PREFIX##MI_MSUBw, \
	PREFIX##MI_MAXx  , PREFIX##MI_MAXy  , PREFIX##MI_MAXz  , PREFIX##MI_MAXw,  /* 0x10 */  \
	PREFIX##MI_MINIx , PREFIX##MI_MINIy , PREFIX##MI_MINIz , PREFIX##MI_MINIw, \
	PREFIX##MI_MULx  , PREFIX##MI_MULy  , PREFIX##MI_MULz  , PREFIX##MI_MULw, \
	PREFIX##MI_MULq  , PREFIX##MI_MAXi  , PREFIX##MI_MULi  , PREFIX##MI_MINIi, \
	PREFIX##MI_ADDq  , PREFIX##MI_MADDq , PREFIX##MI_ADDi  , PREFIX##MI_MADDi, /* 0x20 */ \
	PREFIX##MI_SUBq  , PREFIX##MI_MSUBq , PREFIX##MI_SUBi  , PREFIX##MI_MSUBi, \
	PREFIX##MI_ADD   , PREFIX##MI_MADD  , PREFIX##MI_MUL   , PREFIX##MI_MAX, \
	PREFIX##MI_SUB   , PREFIX##MI_MSUB  , PREFIX##MI_OPMSUB, PREFIX##MI_MINI, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown,  /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##_UPPER_FD_00, PREFIX##_UPPER_FD_01, PREFIX##_UPPER_FD_10, PREFIX##_UPPER_FD_11,  \
};


#define _vuTablesPre(VU, PREFIX) \
 \
 static void PREFIX##_UPPER_FD_00(); \
 static void PREFIX##_UPPER_FD_01(); \
 static void PREFIX##_UPPER_FD_10(); \
 static void PREFIX##_UPPER_FD_11(); \
 static void PREFIX##LowerOP(); \
 static void PREFIX##LowerOP_T3_00(); \
 static void PREFIX##LowerOP_T3_01(); \
 static void PREFIX##LowerOP_T3_10(); \
 static void PREFIX##LowerOP_T3_11(); \

#define _vuTablesPost(VU, PREFIX) \
 \
 static void PREFIX##_UPPER_FD_00() { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](); \
} \
 \
 static void PREFIX##_UPPER_FD_01() { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##_UPPER_FD_10() { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##_UPPER_FD_11() { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP() { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](); \
} \
 \
 static void PREFIX##LowerOP_T3_00() { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_01() { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_10() { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_11() { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](); \
} \


// --------------------------------------------------------------------------------------
//  VuRegsN Tables
// --------------------------------------------------------------------------------------

#define _vuRegsTables(VU, PREFIX, FNTYPE) \
 static void PREFIX##_UPPER_FD_00(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_01(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_10(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_11(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_00(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_01(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_10(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_11(_VURegsNum *VUregsn); \
 \
 _vuTablesMess(PREFIX, FNTYPE) \
 \
 static void PREFIX##_UPPER_FD_00(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_01(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_10(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_11(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_00(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_01(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_10(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_11(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \

_vuTablesPre(VU0, VU0)
_vuTablesMess(VU0, FnPtr_VuVoid)
_vuTablesPost(vuRegs[0], VU0)

_vuTablesPre(VU1, VU1)
_vuTablesMess(VU1, FnPtr_VuVoid)
_vuTablesPost(vuRegs[1], VU1)

_vuRegsTables(vuRegs[0], VU0regs, FnPtr_VuRegsN)
_vuRegsTables(vuRegs[1], VU1regs, FnPtr_VuRegsN)

// --------------------------------------------------------------------------------------
//  VU0macro (COP2)
// --------------------------------------------------------------------------------------

#define SYNCMSFLAGS() \
	vuRegs[0].VI[REG_STATUS_FLAG].UL = (vuRegs[0].VI[REG_STATUS_FLAG].UL & 0xFC0) | (vuRegs[0].statusflag & 0xF) | ((vuRegs[0].statusflag & 0xF) << 6); \
	vuRegs[0].VI[REG_MAC_FLAG].UL = vuRegs[0].macflag

#define SYNCCLIPFLAG() vuRegs[0].VI[REG_CLIP_FLAG].UL = vuRegs[0].clipflag

#define SYNCSTATUSFLAG() vuRegs[0].VI[REG_STATUS_FLAG].UL = (vuRegs[0].VI[REG_STATUS_FLAG].UL & 0xFC0) | (vuRegs[0].statusflag & 0xF) | ((vuRegs[0].statusflag & 0xF) << 6)

#define SYNCFDIV() \
	vuRegs[0].VI[REG_Q].UL = vuRegs[0].q.UL; \
	vuRegs[0].VI[REG_STATUS_FLAG].UL = (vuRegs[0].VI[REG_STATUS_FLAG].UL & 0x3CF) | (vuRegs[0].statusflag & 0x30) | ((vuRegs[0].statusflag & 0x30) << 6)

void VABS()  { vuRegs[0].code = cpuRegs.code; _vuABS(&vuRegs[0]); }
void VADD()  { vuRegs[0].code = cpuRegs.code; _vuADD(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDi() { vuRegs[0].code = cpuRegs.code; _vuADDi(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDq() { vuRegs[0].code = cpuRegs.code; _vuADDq(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDx() { vuRegs[0].code = cpuRegs.code; _vuADDx(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDy() { vuRegs[0].code = cpuRegs.code; _vuADDy(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDz() { vuRegs[0].code = cpuRegs.code; _vuADDz(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDw() { vuRegs[0].code = cpuRegs.code; _vuADDw(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDA() { vuRegs[0].code = cpuRegs.code; _vuADDA(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAi() { vuRegs[0].code = cpuRegs.code; _vuADDAi(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAq() { vuRegs[0].code = cpuRegs.code; _vuADDAq(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAx() { vuRegs[0].code = cpuRegs.code; _vuADDAx(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAy() { vuRegs[0].code = cpuRegs.code; _vuADDAy(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAz() { vuRegs[0].code = cpuRegs.code; _vuADDAz(&vuRegs[0]); SYNCMSFLAGS(); }
void VADDAw() { vuRegs[0].code = cpuRegs.code; _vuADDAw(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUB()  { vuRegs[0].code = cpuRegs.code; _vuSUB(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBi() { vuRegs[0].code = cpuRegs.code; _vuSUBi(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBq() { vuRegs[0].code = cpuRegs.code; _vuSUBq(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBx() { vuRegs[0].code = cpuRegs.code; _vuSUBx(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBy() { vuRegs[0].code = cpuRegs.code; _vuSUBy(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBz() { vuRegs[0].code = cpuRegs.code; _vuSUBz(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBw() { vuRegs[0].code = cpuRegs.code; _vuSUBw(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBA()  { vuRegs[0].code = cpuRegs.code; _vuSUBA(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAi() { vuRegs[0].code = cpuRegs.code; _vuSUBAi(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAq() { vuRegs[0].code = cpuRegs.code; _vuSUBAq(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAx() { vuRegs[0].code = cpuRegs.code; _vuSUBAx(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAy() { vuRegs[0].code = cpuRegs.code; _vuSUBAy(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAz() { vuRegs[0].code = cpuRegs.code; _vuSUBAz(&vuRegs[0]); SYNCMSFLAGS(); }
void VSUBAw() { vuRegs[0].code = cpuRegs.code; _vuSUBAw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMUL()  { vuRegs[0].code = cpuRegs.code; _vuMUL(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULi() { vuRegs[0].code = cpuRegs.code; _vuMULi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULq() { vuRegs[0].code = cpuRegs.code; _vuMULq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULx() { vuRegs[0].code = cpuRegs.code; _vuMULx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULy() { vuRegs[0].code = cpuRegs.code; _vuMULy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULz() { vuRegs[0].code = cpuRegs.code; _vuMULz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULw() { vuRegs[0].code = cpuRegs.code; _vuMULw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULA()  { vuRegs[0].code = cpuRegs.code; _vuMULA(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAi() { vuRegs[0].code = cpuRegs.code; _vuMULAi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAq() { vuRegs[0].code = cpuRegs.code; _vuMULAq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAx() { vuRegs[0].code = cpuRegs.code; _vuMULAx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAy() { vuRegs[0].code = cpuRegs.code; _vuMULAy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAz() { vuRegs[0].code = cpuRegs.code; _vuMULAz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMULAw() { vuRegs[0].code = cpuRegs.code; _vuMULAw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADD()  { vuRegs[0].code = cpuRegs.code; _vuMADD(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDi() { vuRegs[0].code = cpuRegs.code; _vuMADDi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDq() { vuRegs[0].code = cpuRegs.code; _vuMADDq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDx() { vuRegs[0].code = cpuRegs.code; _vuMADDx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDy() { vuRegs[0].code = cpuRegs.code; _vuMADDy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDz() { vuRegs[0].code = cpuRegs.code; _vuMADDz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDw() { vuRegs[0].code = cpuRegs.code; _vuMADDw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDA()  { vuRegs[0].code = cpuRegs.code; _vuMADDA(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAi() { vuRegs[0].code = cpuRegs.code; _vuMADDAi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAq() { vuRegs[0].code = cpuRegs.code; _vuMADDAq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAx() { vuRegs[0].code = cpuRegs.code; _vuMADDAx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAy() { vuRegs[0].code = cpuRegs.code; _vuMADDAy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAz() { vuRegs[0].code = cpuRegs.code; _vuMADDAz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMADDAw() { vuRegs[0].code = cpuRegs.code; _vuMADDAw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUB()  { vuRegs[0].code = cpuRegs.code; _vuMSUB(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBi() { vuRegs[0].code = cpuRegs.code; _vuMSUBi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBq() { vuRegs[0].code = cpuRegs.code; _vuMSUBq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBx() { vuRegs[0].code = cpuRegs.code; _vuMSUBx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBy() { vuRegs[0].code = cpuRegs.code; _vuMSUBy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBz() { vuRegs[0].code = cpuRegs.code; _vuMSUBz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBw() { vuRegs[0].code = cpuRegs.code; _vuMSUBw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBA()  { vuRegs[0].code = cpuRegs.code; _vuMSUBA(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAi() { vuRegs[0].code = cpuRegs.code; _vuMSUBAi(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAq() { vuRegs[0].code = cpuRegs.code; _vuMSUBAq(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAx() { vuRegs[0].code = cpuRegs.code; _vuMSUBAx(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAy() { vuRegs[0].code = cpuRegs.code; _vuMSUBAy(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAz() { vuRegs[0].code = cpuRegs.code; _vuMSUBAz(&vuRegs[0]); SYNCMSFLAGS(); }
void VMSUBAw() { vuRegs[0].code = cpuRegs.code; _vuMSUBAw(&vuRegs[0]); SYNCMSFLAGS(); }
void VMAX()  { vuRegs[0].code = cpuRegs.code; _vuMAX(&vuRegs[0]); }
void VMAXi() { vuRegs[0].code = cpuRegs.code; _vuMAXi(&vuRegs[0]); }
void VMAXx() { vuRegs[0].code = cpuRegs.code; _vuMAXx(&vuRegs[0]); }
void VMAXy() { vuRegs[0].code = cpuRegs.code; _vuMAXy(&vuRegs[0]); }
void VMAXz() { vuRegs[0].code = cpuRegs.code; _vuMAXz(&vuRegs[0]); }
void VMAXw() { vuRegs[0].code = cpuRegs.code; _vuMAXw(&vuRegs[0]); }
void VMINI()  { vuRegs[0].code = cpuRegs.code; _vuMINI(&vuRegs[0]); }
void VMINIi() { vuRegs[0].code = cpuRegs.code; _vuMINIi(&vuRegs[0]); }
void VMINIx() { vuRegs[0].code = cpuRegs.code; _vuMINIx(&vuRegs[0]); }
void VMINIy() { vuRegs[0].code = cpuRegs.code; _vuMINIy(&vuRegs[0]); }
void VMINIz() { vuRegs[0].code = cpuRegs.code; _vuMINIz(&vuRegs[0]); }
void VMINIw() { vuRegs[0].code = cpuRegs.code; _vuMINIw(&vuRegs[0]); }
void VOPMULA() { vuRegs[0].code = cpuRegs.code; _vuOPMULA(&vuRegs[0]); SYNCMSFLAGS(); }
void VOPMSUB() { vuRegs[0].code = cpuRegs.code; _vuOPMSUB(&vuRegs[0]); SYNCMSFLAGS(); }
void VNOP()    { vuRegs[0].code = cpuRegs.code; _vuNOP(&vuRegs[0]); }
void VFTOI0()  { vuRegs[0].code = cpuRegs.code; _vuFTOI0(&vuRegs[0]); }
void VFTOI4()  { vuRegs[0].code = cpuRegs.code; _vuFTOI4(&vuRegs[0]); }
void VFTOI12() { vuRegs[0].code = cpuRegs.code; _vuFTOI12(&vuRegs[0]); }
void VFTOI15() { vuRegs[0].code = cpuRegs.code; _vuFTOI15(&vuRegs[0]); }
void VITOF0()  { vuRegs[0].code = cpuRegs.code; _vuITOF0(&vuRegs[0]); }
void VITOF4()  { vuRegs[0].code = cpuRegs.code; _vuITOF4(&vuRegs[0]); }
void VITOF12() { vuRegs[0].code = cpuRegs.code; _vuITOF12(&vuRegs[0]); }
void VITOF15() { vuRegs[0].code = cpuRegs.code; _vuITOF15(&vuRegs[0]); }
void VCLIPw()  { vuRegs[0].code = cpuRegs.code; _vuCLIP(&vuRegs[0]); SYNCCLIPFLAG(); }

void VDIV()    { vuRegs[0].code = cpuRegs.code; _vuDIV(&vuRegs[0]); SYNCFDIV(); }
void VSQRT()   { vuRegs[0].code = cpuRegs.code; _vuSQRT(&vuRegs[0]); SYNCFDIV(); }
void VRSQRT()  { vuRegs[0].code = cpuRegs.code; _vuRSQRT(&vuRegs[0]); SYNCFDIV(); }
void VIADD()   { vuRegs[0].code = cpuRegs.code; _vuIADD(&vuRegs[0]); }
void VIADDI()  { vuRegs[0].code = cpuRegs.code; _vuIADDI(&vuRegs[0]); }
void VIADDIU() { vuRegs[0].code = cpuRegs.code; _vuIADDIU(&vuRegs[0]); }
void VIAND()   { vuRegs[0].code = cpuRegs.code; _vuIAND(&vuRegs[0]); }
void VIOR()    { vuRegs[0].code = cpuRegs.code; _vuIOR(&vuRegs[0]); }
void VISUB()   { vuRegs[0].code = cpuRegs.code; _vuISUB(&vuRegs[0]); }
void VISUBIU() { vuRegs[0].code = cpuRegs.code; _vuISUBIU(&vuRegs[0]); }
void VMOVE()   { vuRegs[0].code = cpuRegs.code; _vuMOVE(&vuRegs[0]); }
void VMFIR()   { vuRegs[0].code = cpuRegs.code; _vuMFIR(&vuRegs[0]); }
void VMTIR()   { vuRegs[0].code = cpuRegs.code; _vuMTIR(&vuRegs[0]); }
void VMR32()   { vuRegs[0].code = cpuRegs.code; _vuMR32(&vuRegs[0]); }
void VLQ()     { vuRegs[0].code = cpuRegs.code; _vuLQ(&vuRegs[0]); }
void VLQD()    { vuRegs[0].code = cpuRegs.code; _vuLQD(&vuRegs[0]); }
void VLQI()    { vuRegs[0].code = cpuRegs.code; _vuLQI(&vuRegs[0]); }
void VSQ()     { vuRegs[0].code = cpuRegs.code; _vuSQ(&vuRegs[0]); }
void VSQD()    { vuRegs[0].code = cpuRegs.code; _vuSQD(&vuRegs[0]); }
void VSQI()    { vuRegs[0].code = cpuRegs.code; _vuSQI(&vuRegs[0]); }
void VILW()    { vuRegs[0].code = cpuRegs.code; _vuILW(&vuRegs[0]); }
void VISW()    { vuRegs[0].code = cpuRegs.code; _vuISW(&vuRegs[0]); }
void VILWR()   { vuRegs[0].code = cpuRegs.code; _vuILWR(&vuRegs[0]); }
void VISWR()   { vuRegs[0].code = cpuRegs.code; _vuISWR(&vuRegs[0]); }
void VRINIT()  { vuRegs[0].code = cpuRegs.code; vuRegs[0].VI[REG_R].UL = 0x3F800000 | (vuRegs[0].VF[((vuRegs[0].code >> 11) & 0x1F)].UL[((vuRegs[0].code >> 21) & 0x03)] & 0x007FFFFF); }
void VRGET()   { vuRegs[0].code = cpuRegs.code; _vuRGET(&vuRegs[0]); }
void VRNEXT()  { vuRegs[0].code = cpuRegs.code; _vuRNEXT(&vuRegs[0]); }
void VRXOR()   { vuRegs[0].code = cpuRegs.code; vuRegs[0].VI[REG_R].UL = 0x3F800000 | ((vuRegs[0].VI[REG_R].UL ^ vuRegs[0].VF[((vuRegs[0].code >> 11) & 0x1F)].UL[((vuRegs[0].code >> 21) & 0x03)]) & 0x007FFFFF); }
void VWAITQ()  { vuRegs[0].code = cpuRegs.code; }
void VFSAND()  { vuRegs[0].code = cpuRegs.code; _vuFSAND(&vuRegs[0]); }
void VFSEQ()   { vuRegs[0].code = cpuRegs.code; _vuFSEQ(&vuRegs[0]); }
void VFSOR()   { vuRegs[0].code = cpuRegs.code; _vuFSOR(&vuRegs[0]); }
void VFSSET()  { vuRegs[0].code = cpuRegs.code; _vuFSSET(&vuRegs[0]); SYNCSTATUSFLAG(); }
void VFMAND()  { vuRegs[0].code = cpuRegs.code; _vuFMAND(&vuRegs[0]); }
void VFMEQ()   { vuRegs[0].code = cpuRegs.code; _vuFMEQ(&vuRegs[0]); }
void VFMOR()   { vuRegs[0].code = cpuRegs.code; _vuFMOR(&vuRegs[0]); }
void VFCAND()  { vuRegs[0].code = cpuRegs.code; _vuFCAND(&vuRegs[0]); }
void VFCEQ()   { vuRegs[0].code = cpuRegs.code; _vuFCEQ(&vuRegs[0]); }
void VFCOR()   { vuRegs[0].code = cpuRegs.code; _vuFCOR(&vuRegs[0]); }
void VFCSET()  { vuRegs[0].code = cpuRegs.code; vuRegs[0].clipflag = (u32)(vuRegs[0].code & 0xFFFFFF); SYNCCLIPFLAG(); }
void VFCGET()  { vuRegs[0].code = cpuRegs.code; _vuFCGET(&vuRegs[0]); }
void VXITOP()  { vuRegs[0].code = cpuRegs.code; _vuXITOP(&vuRegs[0]); }

