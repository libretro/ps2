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
#include <list>

#include "Gif_Unit.h"
#include "Counters.h"
#include "Config.h"

alignas(16) u8 g_RealGSMem[Ps2MemSize::GSregs];
bool s_GSRegistersWritten = false;

void gsSetVideoMode(GS_VideoMode mode)
{
	gsVideoMode = mode;
	UpdateVSyncRate(false);
}

// Make sure framelimiter options are in sync with GS capabilities.
void gsReset(void)
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

void gsUpdateFrequency(Pcsx2Config& config)
{
	UpdateVSyncRate(true);
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET)
	{
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		/* Privilage registers also reset. */
		memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR.reset();
		CSRreg.Reset();
		MTGS::ResetGS(false);
	}

	if(csr.SIGNAL)
	{
		/* SIGNAL : What's not known here is whether
		 * or not the SIGID register should be updated
		 * here or when the IMR is cleared (below). */

		if (gifUnit.gsSIGNAL.queued) /* Firing pending signal */
		{
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK)
				gsIrq();
			CSRreg.SIGNAL    = true; // Just to be sure :p
		}
		else
			CSRreg.SIGNAL    = false;
		gifUnit.gsSIGNAL.queued  = false;
		gifUnit.Execute(false, true); // Resume paused transfers
	}

	if (csr.FINISH)
	{
		CSRreg.FINISH = false;
		gifUnit.gsFINISH.gsFINISHFired   = false; //Clear the previously fired FINISH (YS, Indiecar 2005, MGS3)
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	CSRreg.HSINT	= false;
	if(csr.VSINT)	CSRreg.VSINT	= false;
	if(csr.EDWINT)	CSRreg.EDWINT	= false;
}

__fi void gsWrite8(u32 mem, u8 value)
{
	tGS_CSR tmp;
	tmp._u32 = value;
	switch (mem)
	{
		// CSR 8-bit write handlers.
		// I'm quite sure these would just write the CSR portion with the other
		// bits set to 0 (no action).  The previous implementation masked the 8-bit
		// write value against the previous CSR write value, but that really doesn't
		// make any sense, given that the real hardware's CSR circuit probably has no
		// real "memory" where it saves anything.  (for example, you can't write to
		// and change the GS revision or ID portions -- they're all hard wired.) --air

		case GS_CSR: // GS_CSR
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 1: // GS_CSR
			tmp._u32 <<= 8;
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 2: // GS_CSR
			tmp._u32 <<= 16;
			gsCSRwrite(tmp);
			break;
		case GS_CSR + 3: // GS_CSR
			tmp._u32 <<= 24;
			gsCSRwrite(tmp);
			break;
		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// GS Write 16 bit

__fi void gsWrite16(u32 mem, u16 value)
{
	tGS_CSR tmp;
	tmp._u32 = value;
	switch (mem)
	{
		// See note above about CSR 8 bit writes, and handling them as zero'd bits
		// for all but the written parts.
		
		case GS_CSR+2:
			tmp._u32 <<= 16;
			// fallthrough
		case GS_CSR:
			gsCSRwrite(tmp);
			return; // do not write to MTGS memory
		case GS_IMR:
			if ((CSRreg._u32 & 0x1f) & (~value & GSIMR._u32) >> 8)
				gsIrq();
			GSIMR._u32 = (value & 0x1f00)|0x6000;
			return; // do not write to MTGS memory
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 32 bit

__fi void gsWrite32(u32 mem, u32 value)
{
	if (mem == GS_CSR)
	{
		tGS_CSR tmp;
		tmp._u32 = value;
		gsCSRwrite(tmp);
	}
	else if (GS_IMR)
	{
		if ((CSRreg._u32 & 0x1f) & (~value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (value & 0x1f00)|0x6000;
	}
	else
		*(u32*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 64 bit

void gsWrite64_generic( u32 mem, u64 value )
{
	memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

void gsWrite64_page_00( u32 mem, u64 value )
{
	s_GSRegistersWritten |= (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE);

	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			UpdateVSyncRate(false);
	}

	memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

void gsWrite64_page_01( u32 mem, u64 value )
{
	tGS_CSR tmp;
	if (mem == GS_BUSDIR)
	{
		gifUnit.stat.DIR = static_cast<u32>(value) & 1;
		if (gifUnit.stat.DIR) /* Assume will do local->host transfer */
		{
			gifUnit.stat.OPH = true; /* Should we set OPH here? */
			gifUnit.FlushToMTGS(); /* Send any pending GS Primitives to the GS */
		}

		memcpy(PS2GS_BASE(mem), &value, sizeof(value));
	}
	else if (mem == GS_CSR)
	{
		tmp._u64 = value;
		gsCSRwrite(tmp);
	}
	else if (mem == GS_IMR)
	{
		u32 _value = static_cast<u32>(value);
		if ((CSRreg._u32 & 0x1f) & (~_value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (_value & 0x1f00)|0x6000;
	}
	else
	{
		memcpy(PS2GS_BASE(mem), &value, sizeof(value));
	}
}

//////////////////////////////////////////////////////////////////////////
// GS Write 128 bit

void TAKES_R128 gsWrite128_page_00( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	r128_store(PS2GS_BASE(mem), value);
}

void TAKES_R128 gsWrite128_page_01( u32 mem, r128 value )
{
	tGS_CSR tmp;
	if (mem == GS_CSR)
	{
		tmp._u32 = r128_to_u32(value);
		gsCSRwrite(tmp);
	}
	else if (mem == GS_IMR)
	{
		u32 _value = r128_to_u32(value);
		if ((CSRreg._u32 & 0x1f) & (~_value & GSIMR._u32) >> 8)
			gsIrq();
		GSIMR._u32 = (_value & 0x1f00)|0x6000;
	}
	else
	{
		alignas(16) const u128 uvalue = r128_to_u128(value);
		r128_store(PS2GS_BASE(mem), value);
	}
}

void TAKES_R128 gsWrite128_generic( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	r128_store(PS2GS_BASE(mem), value);
}

__fi u8 gsRead8(u32 mem)
{
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u8*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
	}
}

__fi u16 gsRead16(u32 mem)
{
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u16*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
	}
}

__fi u32 gsRead32(u32 mem)
{
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u32*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
	}
}

__fi u64 gsRead64(u32 mem)
{
	// fixme - PS2GS_BASE(mem+4) = (g_RealGSMem+(mem + 4 & 0x13ff))
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u64*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
	}
}

__fi u128 gsNonMirroredRead(u32 mem)
{
	return *(u128*)PS2GS_BASE(mem);
}

void gsIrq(void) { hwIntcIrq(INTC_GS); }

//These are done at VSync Start.  Drawing is done when VSync is off, then output the screen when Vsync is on
//The GS needs to be told at the start of a vsync else it loses half of its picture (could be responsible for some halfscreen issues)
//We got away with it before i think due to our awful GS timing, but now we have it right (ish)
void gsPostVsyncStart(void)
{
	MTGS::PostVsyncStart();
}

bool SaveStateBase::gsFreeze()
{
	FreezeMem(PS2MEM_GS, 0x2000);
	Freeze(gsVideoMode);

	return IsOkay();
}

