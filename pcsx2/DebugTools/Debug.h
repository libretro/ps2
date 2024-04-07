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

#include "common/TraceLog.h"
#include "Config.h"
#include "Memory.h"

extern FILE *emuLog;
extern std::string emuLogName;

extern char* disVU0MicroUF(u32 code, u32 pc);
extern char* disVU0MicroLF(u32 code, u32 pc);
extern char* disVU1MicroUF(u32 code, u32 pc);
extern char* disVU1MicroLF(u32 code, u32 pc);

namespace R5900
{
	void disR5900Fasm( std::string& output, u32 code, u32 pc, bool simplify = false);

	extern const char * const GPR_REG[32];
	extern const char * const COP0_REG[32];
	extern const char * const COP1_REG_FP[32];
	extern const char * const COP1_REG_FCR[32];
	extern const char * const COP2_REG_FP[32];
	extern const char * const COP2_REG_CTL[32];
	extern const char * const COP2_VFnames[4];
	extern const char * const GS_REG_PRIV[19];
	extern const u32 GS_REG_PRIV_ADDR[19];
}

namespace R3000A
{
	extern void (*IOP_DEBUG_BSC[64])(char *buf);

	extern const char * const disRNameGPR[];
	extern char* disR3000AF(u32 code, u32 pc);
}

// this structure uses old fashioned C-style "polymorphism".  The base struct TraceLogDescriptor
// must always be the first member in the struct.
struct SysTraceLogDescriptor
{
	TraceLogDescriptor	base;
	const char*			Prefix;
};

// --------------------------------------------------------------------------------------
//  SysTraceLog
// --------------------------------------------------------------------------------------
// Default trace log for high volume VM/System logging.
// This log dumps to emuLog.txt directly and has no ability to pipe output
// to the console (due to the console's inability to handle extremely high
// logging volume).
class SysTraceLog : public TextFileTraceLog
{
public:
	// Pass me a NULL and you *will* suffer!  Muahahaha.
	SysTraceLog( const SysTraceLogDescriptor* desc )
		: TextFileTraceLog( &desc->base ) {}

	void DoWrite( const char *fmt ) const override;
	bool IsActive() const override
	{
		return EmuConfig.Trace.Enabled && Enabled;
	}
};

class SysTraceLog_EE : public SysTraceLog
{
	typedef SysTraceLog _parent;

public:
	SysTraceLog_EE( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	void ApplyPrefix( std::string& ascii ) const override;
	bool IsActive() const override
	{
		return SysTraceLog::IsActive() && EmuConfig.Trace.EE.m_EnableAll;
	}
};

class SysTraceLog_VIFcode : public SysTraceLog_EE
{
	typedef SysTraceLog_EE _parent;

public:
	SysTraceLog_VIFcode( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	void ApplyPrefix(std::string& ascii ) const override;
};

class SysTraceLog_EE_Disasm : public SysTraceLog_EE
{
	typedef SysTraceLog_EE _parent;

public:
	SysTraceLog_EE_Disasm( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.EE.m_EnableDisasm;
	}
};

class SysTraceLog_EE_Registers : public SysTraceLog_EE
{
	typedef SysTraceLog_EE _parent;

public:
	SysTraceLog_EE_Registers( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.EE.m_EnableRegisters;
	}
};

class SysTraceLog_EE_Events : public SysTraceLog_EE
{
	typedef SysTraceLog_EE _parent;

public:
	SysTraceLog_EE_Events( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.EE.m_EnableEvents;
	}
};


class SysTraceLog_IOP : public SysTraceLog
{
	typedef SysTraceLog _parent;

public:
	SysTraceLog_IOP( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}

	void ApplyPrefix( std::string& ascii ) const override;
	bool IsActive() const override
	{
		return SysTraceLog::IsActive() && EmuConfig.Trace.IOP.m_EnableAll;
	}
};

class SysTraceLog_IOP_Disasm : public SysTraceLog_IOP
{
	typedef SysTraceLog_IOP _parent;

public:
	SysTraceLog_IOP_Disasm( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}
	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.IOP.m_EnableDisasm;
	}
};

class SysTraceLog_IOP_Registers : public SysTraceLog_IOP
{
	typedef SysTraceLog_IOP _parent;

public:
	SysTraceLog_IOP_Registers( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}
	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.IOP.m_EnableRegisters;
	}
};

class SysTraceLog_IOP_Events : public SysTraceLog_IOP
{
	typedef SysTraceLog_IOP _parent;

public:
	SysTraceLog_IOP_Events( const SysTraceLogDescriptor* desc ) : _parent( desc ) {}
	bool IsActive() const override
	{
		return _parent::IsActive() && EmuConfig.Trace.IOP.m_EnableEvents;
	}
};

// --------------------------------------------------------------------------------------
//  ConsoleLogFromVM
// --------------------------------------------------------------------------------------
// Special console logger for Virtual Machine log sources, such as the EE and IOP console
// writes (actual game developer messages and such).  These logs do *not* automatically
// append newlines, since the VM generates them manually; and they do *not* support printf
// formatting, since anything coming over the EE/IOP consoles should be considered raw
// string data.  (otherwise %'s would get mis-interpreted).
//
template< ConsoleColors conColor >
class ConsoleLogFromVM : public BaseTraceLogSource
{
	typedef BaseTraceLogSource _parent;

public:
	ConsoleLogFromVM( const TraceLogDescriptor* desc ) : _parent( desc ) {}

	bool Write( const char* msg ) const
	{
		ConsoleColorScope cs(conColor);
		Console.WriteRaw(msg);

		// Buffered output isn't compatible with the testsuite. The end of test
		// doesn't always get flushed. Let's just flush all the output if EE/IOP
		// print anything.
		fflush(NULL);

		return false;
	}

	bool Write(const std::string& msg) const
	{
		return Write(msg.c_str());
	}
};

// --------------------------------------------------------------------------------------
//  SysTraceLogPack
// --------------------------------------------------------------------------------------
struct SysTraceLogPack
{
	// TODO : Sif has special logging needs.. ?
	SysTraceLog	SIF;

	struct EE_PACK
	{
		SysTraceLog_EE				Bios;
		SysTraceLog_EE				Memory;
		SysTraceLog_EE				GIFtag;
		SysTraceLog_VIFcode			VIFcode;
		SysTraceLog_EE      		MSKPATH3;

		SysTraceLog_EE_Disasm		R5900;
		SysTraceLog_EE_Disasm		COP0;
		SysTraceLog_EE_Disasm		COP1;
		SysTraceLog_EE_Disasm		COP2;
		SysTraceLog_EE_Disasm		Cache;

		SysTraceLog_EE_Registers	KnownHw;
		SysTraceLog_EE_Registers	UnknownHw;
		SysTraceLog_EE_Registers	DMAhw;
		SysTraceLog_EE_Registers	IPU;

		SysTraceLog_EE_Events		DMAC;
		SysTraceLog_EE_Events		Counters;
		SysTraceLog_EE_Events		SPR;

		SysTraceLog_EE_Events		VIF;
		SysTraceLog_EE_Events		GIF;

		EE_PACK();
	} EE;

	struct IOP_PACK
	{
		SysTraceLog_IOP				Bios;
		SysTraceLog_IOP				Memcards;
		SysTraceLog_IOP				PAD;

		SysTraceLog_IOP_Disasm		R3000A;
		SysTraceLog_IOP_Disasm		COP2;
		SysTraceLog_IOP_Disasm		Memory;

		SysTraceLog_IOP_Registers	KnownHw;
		SysTraceLog_IOP_Registers	UnknownHw;
		SysTraceLog_IOP_Registers	DMAhw;

		// TODO items to be added, or removed?  I can't remember which! --air
		//SysTraceLog_IOP_Registers	SPU2;
		//SysTraceLog_IOP_Registers	USB;
		//SysTraceLog_IOP_Registers	FW;

		SysTraceLog_IOP_Events		DMAC;
		SysTraceLog_IOP_Events		Counters;
		SysTraceLog_IOP_Events		CDVD;
		SysTraceLog_IOP_Events		MDEC;

		IOP_PACK();
	} IOP;

	SysTraceLogPack();
};

struct SysConsoleLogPack
{
	ConsoleLogSource		ELF;
	ConsoleLogSource		eeRecPerf;
	ConsoleLogSource		sysoutConsole;
	ConsoleLogSource		pgifLog;

	ConsoleLogFromVM<Color_Cyan>		eeConsole;
	ConsoleLogFromVM<Color_Yellow>		iopConsole;
	ConsoleLogFromVM<Color_Cyan>		deci2;
	ConsoleLogFromVM<Color_Red>		controlInfo;

	SysConsoleLogPack();
};


extern SysTraceLogPack SysTrace;
extern SysConsoleLogPack SysConsole;
