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
#include <float.h>

#include "Common.h"

#include "common/StringUtil.h"
#include "BiosTools.h"
#include "R5900.h"
#include "R3000A.h"
#include "IopPgpuGif.h" // PGIF init
#include "VUmicro.h"
#include "COP0.h"
#include "GS.h"
#include "MTVU.h"
#include "VMManager.h"

#include "Hardware.h"
#include "IPUdma.h"

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "vtlb.h"

#include "R5900OpcodeTables.h"

#include "x86/iMMI.h"
#include "x86/iCOP0.h"
#include "x86/iFPU.h"

s32 EEsCycle;		// used to sync the IOP to the EE
u32 EEoCycle;

alignas(16) cpuRegisters cpuRegs;
alignas(16) fpuRegisters fpuRegs;
alignas(16) tlbs tlb[48];
R5900cpu *Cpu = NULL;

bool g_SkipBiosHack; // set at boot if the skip bios hack is on, reset before the game has started
bool g_GameStarted; // set when we reach the game's entry point or earlier if the entry point cannot be determined
bool g_GameLoading; // EELOAD has been called to load the game

static const uint eeWaitCycles = 3072;

bool eeEventTestIsActive = false;
static EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

/* I don't know how much space for args there is in the memory block used for args in full boot mode,
but in fast boot mode, the block we use can fit at least 16 argv pointers (varies with BIOS version).
The second EELOAD call during full boot has three built-in arguments ("EELOAD rom0:PS2LOGO <ELF>"),
meaning that only the first 13 game arguments supplied by the user can be added on and passed through.
In fast boot mode, 15 arguments can fit because the only call to EELOAD is "<ELF> <<args>>". */
#define KMAXARGS 16
static uintptr_t g_argPtrs[KMAXARGS];

extern SysMainMemory& GetVmMemory();

GS_VideoMode gsVideoMode = GS_VideoMode::Uninitialized;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {

	/*********************************************************
	 * Arithmetic with immediate operand                      *
	 * Format:  OP rt, rs, immediate                          *
	 *********************************************************/

	void recADDI(void);
	void recADDIU(void);
	void recDADDI(void);
	void recDADDIU(void);
	void recANDI(void);
	void recORI(void);
	void recXORI(void);

	void recSLTI(void);
	void recSLTIU(void);

	/*********************************************************
	 * Register arithmetic                                    *
	 * Format:  OP rd, rs, rt                                 *
	 *********************************************************/

	void recADD(void);
	void recADDU(void);
	void recDADD(void);
	void recDADDU(void);
	void recSUB(void);
	void recSUBU(void);
	void recDSUB(void);
	void recDSUBU(void);
	void recAND(void);
	void recOR(void);
	void recXOR(void);
	void recNOR(void);
	void recSLT(void);
	void recSLTU(void);

	/*********************************************************
	 * Register mult/div & Register trap logic                *
	 * Format:  OP rs, rt                                     *
	 *********************************************************/
	void recMULT(void);
	void recMULTU(void);
	void recDIV(void);
	void recDIVU(void);

	/*********************************************************
	 * Shift arithmetic with constant shift                   *
	 * Format:  OP rd, rt, sa                                 *
	 *********************************************************/
	void recSLL(void);
	void recSRL(void);
	void recSRA(void);
	void recDSLL(void);
	void recDSRL(void);
	void recDSRA(void);
	void recDSLL32(void);
	void recDSRL32(void);
	void recDSRA32(void);

	void recSLLV(void);
	void recSRLV(void);
	void recSRAV(void);
	void recDSLLV(void);
	void recDSRLV(void);
	void recDSRAV(void);

	/*********************************************************
	 * Shift arithmetic with constant shift                   *
	 * Format:  OP rd, rt, sa                                 *
	 *********************************************************/
	void recBEQ(void);
	void recBEQL(void);
	void recBNE(void);
	void recBNEL(void);
	void recBLTZ(void);
	void recBLTZL(void);
	void recBLTZAL(void);
	void recBLTZALL(void);
	void recBGTZ(void);
	void recBGTZL(void);
	void recBLEZ(void);
	void recBLEZL(void);
	void recBGEZ(void);
	void recBGEZL(void);
	void recBGEZAL(void);
	void recBGEZALL(void);

	/*********************************************************
	 * Jump to target                                         *
	 * Format:  OP target                                     *
	 *********************************************************/
	void recJ(void);
	void recJAL(void);
	void recJR(void);
	void recJALR(void);

	/*********************************************************
	 * Load and store for GPR                                 *
	 * Format:  OP rt, offset(base)                           *
	 *********************************************************/
	void recLB(void);
	void recLBU(void);
	void recLH(void);
	void recLHU(void);
	void recLW(void);
	void recLWU(void);
	void recLWL(void);
	void recLWR(void);
	void recLD(void);
	void recLDR(void);
	void recLDL(void);
	void recLQ(void);
	void recSB(void);
	void recSH(void);
	void recSW(void);
	void recSWL(void);
	void recSWR(void);
	void recSD(void);
	void recSDL(void);
	void recSDR(void);
	void recSQ(void);
	void recLWC1(void);
	void recSWC1(void);
	void recLQC2(void);
	void recSQC2(void);

	void recLUI(void);
	void recMFLO(void);
	void recMFHI(void);
	void recMTLO(void);
	void recMTHI(void);
	void recMOVN(void);
	void recMOVZ(void);

} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
 
namespace R5900
{
	namespace Opcodes
	{
		// Generates an entry for the given opcode name.
		// Assumes the default function naming schemes for interpreter and recompiler  functions.
	#	define MakeOpcode( name, cycles, flags ) \
		static const OPCODE name = { \
			#name, \
			cycles, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::name, \
			::R5900::Dynarec::OpcodeImpl::rec##name \
		}

#	define MakeOpcodeM( name, cycles, flags ) \
		static const OPCODE name = { \
			#name, \
			cycles, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::MMI::name, \
			::R5900::Dynarec::OpcodeImpl::MMI::rec##name \
		}

#	define MakeOpcode0( name, cycles, flags ) \
		static const OPCODE name = { \
			#name, \
			cycles, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP0::name, \
			::R5900::Dynarec::OpcodeImpl::COP0::rec##name \
		}

	#	define MakeOpcode1( name, cycles, flags ) \
		static const OPCODE name = { \
			#name, \
			cycles, \
			flags, \
			NULL, \
			::R5900::Interpreter::OpcodeImpl::COP1::name, \
			::R5900::Dynarec::OpcodeImpl::COP1::rec##name \
		}

	#	define MakeOpcodeClass( name ) \
		static const OPCODE name = { \
			#name, \
			0, \
			0, \
			R5900::Opcodes::Class_##name, \
			NULL, \
			NULL \
		}

		// We're working on new hopefully better cycle ratios, but they're still a WIP.
		// And yes this whole thing is an ugly hack.  I'll clean it up once we have
		// a better idea how exactly the cycle ratios will work best.

		namespace Cycles
		{
			static const int Default = 9;
			static const int Branch = 11;
			static const int CopDefault = 7;

			static const int Mult = 2*8;
			static const int Div = 14*8;
			static const int MMI_Mult = 3*8;
			static const int MMI_Div = 22*8;
			static const int MMI_Default = 14;

			static const int FPU_Mult = 4*8;

			static const int Store = 14;
			static const int Load = 14;
		}

		using namespace Cycles;

		MakeOpcode( Unknown, Default, 0 );
		MakeOpcode( MMI_Unknown, Default, 0 );
		MakeOpcode( COP0_Unknown, Default, 0 );
		MakeOpcode( COP1_Unknown, Default, 0 );

		// Class Subset Opcodes
		// (not really opcodes, but rather entire subsets of other opcode classes)

		MakeOpcodeClass( SPECIAL );
		MakeOpcodeClass( REGIMM );
		MakeOpcodeClass( MMI );
		MakeOpcodeClass( MMI0 );
		MakeOpcodeClass( MMI2 );
		MakeOpcodeClass( MMI1 );
		MakeOpcodeClass( MMI3 );

		MakeOpcodeClass( COP0 );
		MakeOpcodeClass( COP1 );

		// Misc Junk

		MakeOpcode( COP2, Default, 0 );

		MakeOpcode( CACHE, Default, 0 );
		MakeOpcode( PREF, Default, 0 );
		MakeOpcode( SYSCALL, Default, IS_BRANCH|BRANCHTYPE_SYSCALL );
		MakeOpcode( BREAK, Default, 0 );
		MakeOpcode( SYNC, Default, 0 );

		// Branch/Jump Opcodes

		MakeOpcode( J ,   Default, IS_BRANCH|BRANCHTYPE_JUMP );
		MakeOpcode( JAL,  Default, IS_BRANCH|BRANCHTYPE_JUMP|IS_LINKED );
		MakeOpcode( JR,   Default, IS_BRANCH|BRANCHTYPE_REGISTER );
		MakeOpcode( JALR, Default, IS_BRANCH|BRANCHTYPE_REGISTER|IS_LINKED );

		MakeOpcode( BEQ,     Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ );
		MakeOpcode( BNE,     Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE );
		MakeOpcode( BLEZ,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ );
		MakeOpcode( BGTZ,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ );
		MakeOpcode( BEQL,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_EQ|IS_LIKELY );
		MakeOpcode( BNEL,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_NE|IS_LIKELY );
		MakeOpcode( BLEZL,   Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LEZ|IS_LIKELY );
		MakeOpcode( BGTZL,   Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GTZ|IS_LIKELY );
		MakeOpcode( BLTZ,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ );
		MakeOpcode( BGEZ,    Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ );
		MakeOpcode( BLTZL,   Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LIKELY );
		MakeOpcode( BGEZL,   Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LIKELY );
		MakeOpcode( BLTZAL,  Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED );
		MakeOpcode( BGEZAL,  Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED );
		MakeOpcode( BLTZALL, Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_LTZ|IS_LINKED|IS_LIKELY );
		MakeOpcode( BGEZALL, Branch, IS_BRANCH|BRANCHTYPE_BRANCH|CONDTYPE_GEZ|IS_LINKED|IS_LIKELY );

		MakeOpcode( TGEI, Branch, 0 );
		MakeOpcode( TGEIU, Branch, 0 );
		MakeOpcode( TLTI, Branch, 0 );
		MakeOpcode( TLTIU, Branch, 0 );
		MakeOpcode( TEQI, Branch, 0 );
		MakeOpcode( TNEI, Branch, 0 );
		MakeOpcode( TGE, Branch, 0 );
		MakeOpcode( TGEU, Branch, 0 );
		MakeOpcode( TLT, Branch, 0 );
		MakeOpcode( TLTU, Branch, 0 );
		MakeOpcode( TEQ, Branch, 0 );
		MakeOpcode( TNE, Branch, 0 );

		// Arithmetic

		MakeOpcode( MULT, Mult, 0 );
		MakeOpcode( MULTU, Mult, 0 );
		MakeOpcode( MULT1, Mult, 0 );
		MakeOpcode( MULTU1, Mult, 0 );
		MakeOpcode( MADD, Mult, 0 );
		MakeOpcode( MADDU, Mult, 0 );
		MakeOpcode( MADD1, Mult, 0 );
		MakeOpcode( MADDU1, Mult, 0 );
		MakeOpcode( DIV, Div, 0 );
		MakeOpcode( DIVU, Div, 0 );
		MakeOpcode( DIV1, Div, 0 );
		MakeOpcode( DIVU1, Div, 0 );

		MakeOpcode( ADDI,   Default, IS_ALU|ALUTYPE_ADDI );
		MakeOpcode( ADDIU,  Default, IS_ALU|ALUTYPE_ADDI );
		MakeOpcode( DADDI,  Default, IS_ALU|ALUTYPE_ADDI|IS_64BIT );
		MakeOpcode( DADDIU, Default, IS_ALU|ALUTYPE_ADDI|IS_64BIT );
		MakeOpcode( DADD,   Default, IS_ALU|ALUTYPE_ADD|IS_64BIT );
		MakeOpcode( DADDU,  Default, IS_ALU|ALUTYPE_ADD|IS_64BIT );
		MakeOpcode( DSUB,   Default, IS_ALU|ALUTYPE_SUB|IS_64BIT );
		MakeOpcode( DSUBU,  Default, IS_ALU|ALUTYPE_SUB|IS_64BIT );
		MakeOpcode( ADD,    Default, IS_ALU|ALUTYPE_ADD );
		MakeOpcode( ADDU,   Default, IS_ALU|ALUTYPE_ADD );
		MakeOpcode( SUB,    Default, IS_ALU|ALUTYPE_SUB );
		MakeOpcode( SUBU,   Default, IS_ALU|ALUTYPE_SUB );

		MakeOpcode( ANDI, Default, 0 );
		MakeOpcode( ORI, Default, 0 );
		MakeOpcode( XORI, Default, 0 );
		MakeOpcode( AND, Default, 0 );
		MakeOpcode( OR, Default, 0 );
		MakeOpcode( XOR, Default, 0 );
		MakeOpcode( NOR, Default, 0 );
		MakeOpcode( SLTI, Default, 0 );
		MakeOpcode( SLTIU, Default, 0 );
		MakeOpcode( SLT, Default, 0 );
		MakeOpcode( SLTU, Default, 0 );
		MakeOpcode( LUI, Default, 0 );
		MakeOpcode( SLL, Default, 0 );
		MakeOpcode( SRL, Default, 0 );
		MakeOpcode( SRA, Default, 0 );
		MakeOpcode( SLLV, Default, 0 );
		MakeOpcode( SRLV, Default, 0 );
		MakeOpcode( SRAV, Default, 0 );
		MakeOpcode( MOVZ, Default, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_EQ );
		MakeOpcode( MOVN, Default, IS_ALU|ALUTYPE_CONDMOVE|CONDTYPE_NE );
		MakeOpcode( DSLLV, Default, 0 );
		MakeOpcode( DSRLV, Default, 0 );
		MakeOpcode( DSRAV, Default, 0 );
		MakeOpcode( DSLL, Default, 0 );
		MakeOpcode( DSRL, Default, 0 );
		MakeOpcode( DSRA, Default, 0 );
		MakeOpcode( DSLL32, Default, 0 );
		MakeOpcode( DSRL32, Default, 0 );
		MakeOpcode( DSRA32, Default, 0 );

		MakeOpcode( MFHI, Default, 0 );
		MakeOpcode( MTHI, Default, 0 );
		MakeOpcode( MFLO, Default, 0 );
		MakeOpcode( MTLO, Default, 0 );
		MakeOpcode( MFSA, Default, 0 );
		MakeOpcode( MTSA, Default, 0 );
		MakeOpcode( MTSAB, Default, 0 );
		MakeOpcode( MTSAH, Default, 0 );
		MakeOpcode( MFHI1, Default, 0 );
		MakeOpcode( MTHI1, Default, 0 );
		MakeOpcode( MFLO1, Default, 0 );
		MakeOpcode( MTLO1, Default, 0 );

		// Loads!

		MakeOpcode( LDL,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_LEFT );
		MakeOpcode( LDR,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD|IS_RIGHT );
		MakeOpcode( LQ,   Load, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD );
		MakeOpcode( LB,   Load, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE );
		MakeOpcode( LH,   Load, IS_MEMORY|IS_LOAD|MEMTYPE_HALF );
		MakeOpcode( LWL,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_LEFT );
		MakeOpcode( LW,   Load, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LBU,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_BYTE );
		MakeOpcode( LHU,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_HALF );
		MakeOpcode( LWR,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_WORD|IS_RIGHT );
		MakeOpcode( LWU,  Load, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LWC1, Load, IS_MEMORY|IS_LOAD|MEMTYPE_WORD );
		MakeOpcode( LQC2, Load, IS_MEMORY|IS_LOAD|MEMTYPE_QWORD );
		MakeOpcode( LD,   Load, IS_MEMORY|IS_LOAD|MEMTYPE_DWORD );

		// Stores!

		MakeOpcode( SQ,   Store, IS_MEMORY|IS_STORE|MEMTYPE_QWORD );
		MakeOpcode( SB,   Store, IS_MEMORY|IS_STORE|MEMTYPE_BYTE );
		MakeOpcode( SH,   Store, IS_MEMORY|IS_STORE|MEMTYPE_HALF );
		MakeOpcode( SWL,  Store, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_LEFT );
		MakeOpcode( SW,   Store, IS_MEMORY|IS_STORE|MEMTYPE_WORD );
		MakeOpcode( SDL,  Store, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_LEFT );
		MakeOpcode( SDR,  Store, IS_MEMORY|IS_STORE|MEMTYPE_DWORD|IS_RIGHT );
		MakeOpcode( SWR,  Store, IS_MEMORY|IS_STORE|MEMTYPE_WORD|IS_RIGHT );
		MakeOpcode( SWC1, Store, IS_MEMORY|IS_STORE|MEMTYPE_WORD );
		MakeOpcode( SQC2, Store, IS_MEMORY|IS_STORE|MEMTYPE_QWORD );
		MakeOpcode( SD,   Store, IS_MEMORY|IS_STORE|MEMTYPE_DWORD );


		// Multimedia Instructions!

		MakeOpcodeM( PLZCW, MMI_Default, 0 );
		MakeOpcodeM( PMFHL, MMI_Default, 0 );
		MakeOpcodeM( PMTHL, MMI_Default, 0 );
		MakeOpcodeM( PSLLH, MMI_Default, 0 );
		MakeOpcodeM( PSRLH, MMI_Default, 0 );
		MakeOpcodeM( PSRAH, MMI_Default, 0 );
		MakeOpcodeM( PSLLW, MMI_Default, 0 );
		MakeOpcodeM( PSRLW, MMI_Default, 0 );
		MakeOpcodeM( PSRAW, MMI_Default, 0 );

		MakeOpcodeM( PADDW, MMI_Default, 0 );
		MakeOpcodeM( PADDH, MMI_Default, 0 );
		MakeOpcodeM( PADDB, MMI_Default, 0 );
		MakeOpcodeM( PADDSW, MMI_Default, 0 );
		MakeOpcodeM( PADDSH, MMI_Default, 0 );
		MakeOpcodeM( PADDSB, MMI_Default, 0 );
		MakeOpcodeM( PADDUW, MMI_Default, 0 );
		MakeOpcodeM( PADDUH, MMI_Default, 0 );
		MakeOpcodeM( PADDUB, MMI_Default, 0 );
		MakeOpcodeM( PSUBW, MMI_Default, 0 );
		MakeOpcodeM( PSUBH, MMI_Default, 0 );
		MakeOpcodeM( PSUBB, MMI_Default, 0 );
		MakeOpcodeM( PSUBSW, MMI_Default, 0 );
		MakeOpcodeM( PSUBSH, MMI_Default, 0 );
		MakeOpcodeM( PSUBSB, MMI_Default, 0 );
		MakeOpcodeM( PSUBUW, MMI_Default, 0 );
		MakeOpcodeM( PSUBUH, MMI_Default, 0 );
		MakeOpcodeM( PSUBUB, MMI_Default, 0 );

		MakeOpcodeM( PCGTW, MMI_Default, 0 );
		MakeOpcodeM( PMAXW, MMI_Default, 0 );
		MakeOpcodeM( PMAXH, MMI_Default, 0 );
		MakeOpcodeM( PCGTH, MMI_Default, 0 );
		MakeOpcodeM( PCGTB, MMI_Default, 0 );
		MakeOpcodeM( PEXTLW, MMI_Default, 0 );
		MakeOpcodeM( PEXTLH, MMI_Default, 0 );
		MakeOpcodeM( PEXTLB, MMI_Default, 0 );
		MakeOpcodeM( PEXT5, MMI_Default, 0 );
		MakeOpcodeM( PPACW, MMI_Default, 0 );
		MakeOpcodeM( PPACH, MMI_Default, 0 );
		MakeOpcodeM( PPACB, MMI_Default, 0 );
		MakeOpcodeM( PPAC5, MMI_Default, 0 );

		MakeOpcodeM( PABSW, MMI_Default, 0 );
		MakeOpcodeM( PABSH, MMI_Default, 0 );
		MakeOpcodeM( PCEQW, MMI_Default, 0 );
		MakeOpcodeM( PMINW, MMI_Default, 0 );
		MakeOpcodeM( PMINH, MMI_Default, 0 );
		MakeOpcodeM( PADSBH, MMI_Default, 0 );
		MakeOpcodeM( PCEQH, MMI_Default, 0 );
		MakeOpcodeM( PCEQB, MMI_Default, 0 );
		MakeOpcodeM( PEXTUW, MMI_Default, 0 );
		MakeOpcodeM( PEXTUH, MMI_Default, 0 );
		MakeOpcodeM( PEXTUB, MMI_Default, 0 );
		MakeOpcodeM( PSLLVW, MMI_Default, 0 );
		MakeOpcodeM( PSRLVW, MMI_Default, 0 );

		MakeOpcodeM( QFSRV, MMI_Default, 0 );

		MakeOpcodeM( PMADDH, MMI_Mult, 0 );
		MakeOpcodeM( PHMADH, MMI_Mult, 0 );
		MakeOpcodeM( PMSUBH, MMI_Mult, 0 );
		MakeOpcodeM( PHMSBH, MMI_Mult, 0 );
		MakeOpcodeM( PMULTH, MMI_Mult, 0 );
		MakeOpcodeM( PMADDW, MMI_Mult, 0 );
		MakeOpcodeM( PMSUBW, MMI_Mult, 0 );
		MakeOpcodeM( PMFHI, MMI_Mult, 0 );
		MakeOpcodeM( PMFLO, MMI_Mult, 0 );
		MakeOpcodeM( PMULTW, MMI_Mult, 0 );
		MakeOpcodeM( PMADDUW, MMI_Mult, 0 );
		MakeOpcodeM( PMULTUW, MMI_Mult, 0 );
		MakeOpcodeM( PDIVUW, MMI_Div, 0 );
		MakeOpcodeM( PDIVW, MMI_Div, 0 );
		MakeOpcodeM( PDIVBW, MMI_Div, 0 );

		MakeOpcodeM( PINTH, MMI_Default, 0 );
		MakeOpcodeM( PCPYLD, MMI_Default, 0 );
		MakeOpcodeM( PAND, MMI_Default, 0 );
		MakeOpcodeM( PXOR, MMI_Default, 0 );
		MakeOpcodeM( PEXEH, MMI_Default, 0 );
		MakeOpcodeM( PREVH, MMI_Default, 0 );
		MakeOpcodeM( PEXEW, MMI_Default, 0 );
		MakeOpcodeM( PROT3W, MMI_Default, 0 );

		MakeOpcodeM( PSRAVW, MMI_Default, 0 );
		MakeOpcodeM( PMTHI, MMI_Default, 0 );
		MakeOpcodeM( PMTLO, MMI_Default, 0 );
		MakeOpcodeM( PINTEH, MMI_Default, 0 );
		MakeOpcodeM( PCPYUD, MMI_Default, 0 );
		MakeOpcodeM( POR, MMI_Default, 0 );
		MakeOpcodeM( PNOR, MMI_Default, 0 );
		MakeOpcodeM( PEXCH, MMI_Default, 0 );
		MakeOpcodeM( PCPYH, MMI_Default, 0 );
		MakeOpcodeM( PEXCW, MMI_Default, 0 );

		//////////////////////////////////////////////////////////
		// COP0 Instructions

		MakeOpcodeClass( COP0_C0 );
		MakeOpcodeClass( COP0_BC0 );

		MakeOpcode0( MFC0, CopDefault, 0 );
		MakeOpcode0( MTC0, CopDefault, 0 );

		MakeOpcode0(BC0F, Branch, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ);
		MakeOpcode0(BC0T, Branch, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE);
		MakeOpcode0(BC0FL, Branch, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_EQ | IS_LIKELY);
		MakeOpcode0(BC0TL, Branch, IS_BRANCH | BRANCHTYPE_BC0 | CONDTYPE_NE | IS_LIKELY);

		MakeOpcode0( TLBR, CopDefault, 0 );
		MakeOpcode0( TLBWI, CopDefault, 0 );
		MakeOpcode0( TLBWR, CopDefault, 0 );
		MakeOpcode0( TLBP, CopDefault, 0 );
		MakeOpcode0( ERET, CopDefault, IS_BRANCH|BRANCHTYPE_ERET );
		MakeOpcode0( EI, CopDefault, 0 );
		MakeOpcode0( DI, CopDefault, 0 );

		//////////////////////////////////////////////////////////
		// COP1 Instructions!

		MakeOpcodeClass( COP1_BC1 );
		MakeOpcodeClass( COP1_S );
		MakeOpcodeClass( COP1_W );		// contains CVT_S instruction *only*

		MakeOpcode1( MFC1, CopDefault, 0 );
		MakeOpcode1( CFC1, CopDefault, 0 );
		MakeOpcode1( MTC1, CopDefault, 0 );
		MakeOpcode1( CTC1, CopDefault, 0 );

		MakeOpcode1( BC1F,  Branch, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ );
		MakeOpcode1( BC1T,  Branch, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE );
		MakeOpcode1( BC1FL, Branch, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_EQ|IS_LIKELY );
		MakeOpcode1( BC1TL, Branch, IS_BRANCH|BRANCHTYPE_BC1|CONDTYPE_NE|IS_LIKELY );

		MakeOpcode1( ADD_S, CopDefault, 0 );
		MakeOpcode1( ADDA_S, CopDefault, 0 );
		MakeOpcode1( SUB_S, CopDefault, 0 );
		MakeOpcode1( SUBA_S, CopDefault, 0 );

		MakeOpcode1( ABS_S, CopDefault, 0 );
		MakeOpcode1( MOV_S, CopDefault, 0 );
		MakeOpcode1( NEG_S, CopDefault, 0 );
		MakeOpcode1( MAX_S, CopDefault, 0 );
		MakeOpcode1( MIN_S, CopDefault, 0 );

		MakeOpcode1( MUL_S, FPU_Mult, 0 );
		MakeOpcode1( DIV_S, 6*8, 0 );
		MakeOpcode1( SQRT_S, 6*8, 0 );
		MakeOpcode1( RSQRT_S, 8*8, 0 );
		MakeOpcode1( MULA_S, FPU_Mult, 0 );
		MakeOpcode1( MADD_S, FPU_Mult, 0 );
		MakeOpcode1( MSUB_S, FPU_Mult, 0 );
		MakeOpcode1( MADDA_S, FPU_Mult, 0 );
		MakeOpcode1( MSUBA_S, FPU_Mult, 0 );

		MakeOpcode1( C_F, CopDefault, 0 );
		MakeOpcode1( C_EQ, CopDefault, 0 );
		MakeOpcode1( C_LT, CopDefault, 0 );
		MakeOpcode1( C_LE, CopDefault, 0 );

		MakeOpcode1( CVT_S, CopDefault, 0 );
		MakeOpcode1( CVT_W, CopDefault, 0 );
	}

	namespace OpcodeTables
	{
		using namespace Opcodes;

		const OPCODE tbl_Standard[64] =
		{
			SPECIAL,       REGIMM,        J,             JAL,     BEQ,           BNE,     BLEZ,  BGTZ,
			ADDI,          ADDIU,         SLTI,          SLTIU,   ANDI,          ORI,     XORI,  LUI,
			COP0,          COP1,          COP2,          Unknown, BEQL,          BNEL,    BLEZL, BGTZL,
			DADDI,         DADDIU,        LDL,           LDR,     MMI,           Unknown, LQ,    SQ,
			LB,            LH,            LWL,           LW,      LBU,           LHU,     LWR,   LWU,
			SB,            SH,            SWL,           SW,      SDL,           SDR,     SWR,   CACHE,
			Unknown,       LWC1,          Unknown,       PREF,    Unknown,       Unknown, LQC2,  LD,
			Unknown,       SWC1,          Unknown,       Unknown, Unknown,       Unknown, SQC2,  SD
		};

		static const OPCODE tbl_Special[64] =
		{
			SLL,      Unknown,  SRL,      SRA,      SLLV,    Unknown, SRLV,    SRAV,
			JR,       JALR,     MOVZ,     MOVN,     SYSCALL, BREAK,   Unknown, SYNC,
			MFHI,     MTHI,     MFLO,     MTLO,     DSLLV,   Unknown, DSRLV,   DSRAV,
			MULT,     MULTU,    DIV,      DIVU,     Unknown, Unknown, Unknown, Unknown,
			ADD,      ADDU,     SUB,      SUBU,     AND,     OR,      XOR,     NOR,
			MFSA,     MTSA,     SLT,      SLTU,     DADD,    DADDU,   DSUB,    DSUBU,
			TGE,      TGEU,     TLT,      TLTU,     TEQ,     Unknown, TNE,     Unknown,
			DSLL,     Unknown,  DSRL,     DSRA,     DSLL32,  Unknown, DSRL32,  DSRA32
		};

		static const OPCODE tbl_RegImm[32] = {
			BLTZ,   BGEZ,   BLTZL,      BGEZL,   Unknown, Unknown, Unknown, Unknown,
			TGEI,   TGEIU,  TLTI,       TLTIU,   TEQI,    Unknown, TNEI,    Unknown,
			BLTZAL, BGEZAL, BLTZALL,    BGEZALL, Unknown, Unknown, Unknown, Unknown,
			MTSAB,  MTSAH , Unknown,    Unknown, Unknown, Unknown, Unknown, Unknown,
		};

		static const OPCODE tbl_MMI[64] =
		{
			MADD,               MADDU,           MMI_Unknown,          MMI_Unknown,          PLZCW,            MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MMI0,      MMI2,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MFHI1,              MTHI1,           MFLO1,                MTLO1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MULT1,              MULTU1,          DIV1,                 DIVU1,                MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MADD1,              MADDU1,          MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			MMI1,      MMI3,   MMI_Unknown,          MMI_Unknown,          MMI_Unknown,      MMI_Unknown,       MMI_Unknown,          MMI_Unknown,
			PMFHL,              PMTHL,           MMI_Unknown,          MMI_Unknown,          PSLLH,            MMI_Unknown,       PSRLH,                PSRAH,
			MMI_Unknown,        MMI_Unknown,     MMI_Unknown,          MMI_Unknown,          PSLLW,            MMI_Unknown,       PSRLW,                PSRAW,
		};

		static const OPCODE tbl_MMI0[32] =
		{
			PADDW,         PSUBW,         PCGTW,          PMAXW,
			PADDH,         PSUBH,         PCGTH,          PMAXH,
			PADDB,         PSUBB,         PCGTB,          MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,    MMI_Unknown,
			PADDSW,        PSUBSW,        PEXTLW,         PPACW,
			PADDSH,        PSUBSH,        PEXTLH,         PPACH,
			PADDSB,        PSUBSB,        PEXTLB,         PPACB,
			MMI_Unknown,   MMI_Unknown,   PEXT5,          PPAC5,
		};

		static const OPCODE tbl_MMI1[32] =
		{
			MMI_Unknown,   PABSW,         PCEQW,         PMINW,
			PADSBH,        PABSH,         PCEQH,         PMINH,
			MMI_Unknown,   MMI_Unknown,   PCEQB,         MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PADDUW,        PSUBUW,        PEXTUW,        MMI_Unknown,
			PADDUH,        PSUBUH,        PEXTUH,        MMI_Unknown,
			PADDUB,        PSUBUB,        PEXTUB,        QFSRV,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
		};


		static const OPCODE tbl_MMI2[32] =
		{
			PMADDW,        MMI_Unknown,   PSLLVW,        PSRLVW,
			PMSUBW,        MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PMFHI,         PMFLO,         PINTH,         MMI_Unknown,
			PMULTW,        PDIVW,         PCPYLD,        MMI_Unknown,
			PMADDH,        PHMADH,        PAND,          PXOR,
			PMSUBH,        PHMSBH,        MMI_Unknown,   MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   PEXEH,         PREVH,
			PMULTH,        PDIVBW,        PEXEW,         PROT3W,
		};

		static const OPCODE tbl_MMI3[32] =
		{
			PMADDUW,       MMI_Unknown,   MMI_Unknown,   PSRAVW,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			PMTHI,         PMTLO,         PINTEH,        MMI_Unknown,
			PMULTUW,       PDIVUW,        PCPYUD,        MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   POR,           PNOR,
			MMI_Unknown,   MMI_Unknown,   MMI_Unknown,   MMI_Unknown,
			MMI_Unknown,   MMI_Unknown,   PEXCH,         PCPYH,
			MMI_Unknown,   MMI_Unknown,   PEXCW,         MMI_Unknown,
		};

		static const OPCODE tbl_COP0[32] =
		{
			MFC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown, MTC0,         COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_BC0, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_C0,  COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
		};

		static const OPCODE tbl_COP0_BC0[32] =
		{
			BC0F,         BC0T,         BC0FL,        BC0TL,        COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
		};

		static const OPCODE tbl_COP0_C0[64] =
		{
			COP0_Unknown, TLBR,         TLBWI,        COP0_Unknown, COP0_Unknown, COP0_Unknown, TLBWR,        COP0_Unknown,
			TLBP,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			ERET,         COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown,
			EI,           DI,           COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown, COP0_Unknown
		};

		static const OPCODE tbl_COP1[32] =
		{
			MFC1,         COP1_Unknown, CFC1,         COP1_Unknown, MTC1,         COP1_Unknown, CTC1,         COP1_Unknown,
			COP1_BC1, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_S,   COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_W, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
		};

		static const OPCODE tbl_COP1_BC1[32] =
		{
			BC1F,         BC1T,         BC1FL,        BC1TL,        COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
			COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown, COP1_Unknown,
		};

		static const OPCODE tbl_COP1_S[64] =
		{
			ADD_S,       SUB_S,       MUL_S,       DIV_S,       SQRT_S,      ABS_S,       MOV_S,       NEG_S,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,RSQRT_S,     COP1_Unknown,
			ADDA_S,      SUBA_S,      MULA_S,      COP1_Unknown,MADD_S,      MSUB_S,      MADDA_S,     MSUBA_S,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,CVT_W,       COP1_Unknown,COP1_Unknown,COP1_Unknown,
			MAX_S,       MIN_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			C_F,         COP1_Unknown,C_EQ,        COP1_Unknown,C_LT,        COP1_Unknown,C_LE,        COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
		};

		static const OPCODE tbl_COP1_W[64] =
		{
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			CVT_S,       COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
			COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,COP1_Unknown,
		};

	}	// end namespace R5900::OpcodeTables

	namespace Opcodes
	{
		using namespace OpcodeTables;

		const OPCODE& Class_SPECIAL(u32 op) { return tbl_Special[op & 0x3F]; }
		const OPCODE& Class_REGIMM(u32 op)  { return tbl_RegImm[(op >> 16) & 0x1F]; }

		const OPCODE& Class_MMI(u32 op)  { return tbl_MMI[op & 0x3F]; }
		const OPCODE& Class_MMI0(u32 op) { return tbl_MMI0[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI1(u32 op) { return tbl_MMI1[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI2(u32 op) { return tbl_MMI2[(op >> 6) & 0x1F]; }
		const OPCODE& Class_MMI3(u32 op) { return tbl_MMI3[(op >> 6) & 0x1F]; }

		const OPCODE& Class_COP0(u32 op) { return tbl_COP0[(op >> 21) & 0x1F]; }
		const OPCODE& Class_COP0_BC0(u32 op) { return tbl_COP0_BC0[(op >> 16) & 0x03]; }
		const OPCODE& Class_COP0_C0(u32 op) { return tbl_COP0_C0[op & 0x3F]; }

		const OPCODE& Class_COP1(u32 op) { return tbl_COP1[(op >> 21) & 0x1F]; }
		const OPCODE& Class_COP1_BC1(u32 op) { return tbl_COP1_BC1[(op >> 16) & 0x1F]; }
		const OPCODE& Class_COP1_S(u32 op) { return tbl_COP1_S[op & 0x3F]; }
		const OPCODE& Class_COP1_W(u32 op) { return tbl_COP1_W[op & 0x3F]; }
	}
}	/* end namespace R5900 */

void (*Int_COP2PrintTable[32])(void) = {
    COP2_Unknown, QMFC2,        CFC2,         COP2_Unknown, COP2_Unknown, QMTC2,        CTC2,         COP2_Unknown,
    COP2_BC2,     COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
	COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL, COP2_SPECIAL,
};

void (*Int_COP2BC2PrintTable[32])(void) = {
    BC2F,         BC2T,         BC2FL,        BC2TL,        COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
    COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown, COP2_Unknown,
};

void (*Int_COP2SPECIAL1PrintTable[64])(void) =
{
 VADDx,       VADDy,       VADDz,       VADDw,       VSUBx,        VSUBy,        VSUBz,        VSUBw,
 VMADDx,      VMADDy,      VMADDz,      VMADDw,      VMSUBx,       VMSUBy,       VMSUBz,       VMSUBw,
 VMAXx,       VMAXy,       VMAXz,       VMAXw,       VMINIx,       VMINIy,       VMINIz,       VMINIw,
 VMULx,       VMULy,       VMULz,       VMULw,       VMULq,        VMAXi,        VMULi,        VMINIi,
 VADDq,       VMADDq,      VADDi,       VMADDi,      VSUBq,        VMSUBq,       VSUBi,        VMSUBi,
 VADD,        VMADD,       VMUL,        VMAX,        VSUB,         VMSUB,        VOPMSUB,      VMINI,
 VIADD,       VISUB,       VIADDI,      COP2_Unknown,VIAND,        VIOR,         COP2_Unknown, COP2_Unknown,
 VCALLMS,     VCALLMSR,    COP2_Unknown,COP2_Unknown,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,COP2_SPECIAL2,
};

void (*Int_COP2SPECIAL2PrintTable[128])(void) =
{
 VADDAx      ,VADDAy      ,VADDAz      ,VADDAw      ,VSUBAx      ,VSUBAy      ,VSUBAz      ,VSUBAw,
 VMADDAx     ,VMADDAy     ,VMADDAz     ,VMADDAw     ,VMSUBAx     ,VMSUBAy     ,VMSUBAz     ,VMSUBAw,
 VITOF0      ,VITOF4      ,VITOF12     ,VITOF15     ,VFTOI0      ,VFTOI4      ,VFTOI12     ,VFTOI15,
 VMULAx      ,VMULAy      ,VMULAz      ,VMULAw      ,VMULAq      ,VABS        ,VMULAi      ,VCLIPw,
 VADDAq      ,VMADDAq     ,VADDAi      ,VMADDAi     ,VSUBAq      ,VMSUBAq     ,VSUBAi      ,VMSUBAi,
 VADDA       ,VMADDA      ,VMULA       ,COP2_Unknown,VSUBA       ,VMSUBA      ,VOPMULA     ,VNOP,
 VMOVE       ,VMR32       ,COP2_Unknown,COP2_Unknown,VLQI        ,VSQI        ,VLQD        ,VSQD,
 VDIV        ,VSQRT       ,VRSQRT      ,VWAITQ      ,VMTIR       ,VMFIR       ,VILWR       ,VISWR,
 VRNEXT      ,VRGET       ,VRINIT      ,VRXOR       ,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
 COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,COP2_Unknown,
};

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

static __ri void cpuException(u32 code, u32 bd)
{
	bool checkStatus;
	u32 offset          = 0;

	cpuRegs.branch      = 0; // Tells the interpreter that an exception occurred during a branch.
	cpuRegs.CP0.n.Cause = code & 0xffff;

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		//Error Level 0-1
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0); //  for TLB/general exceptions

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = 0x0; //TLB Refill
		else if ((code & 0x7C) == 0x0)
			offset = 0x200; //Interrupt
		else
			offset = 0x180; // Everything else
	}
	else
	{
		//Error Level 2
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0); // for perf/debug exceptions

		if ((code & 0x38000) <= 0x8000 )
		{
			//Reset / NMI
			cpuRegs.pc = 0xBFC00000;
			return;
		}
		else if((code & 0x38000) == 0x10000)
			offset = 0x80; //Performance Counter
		else if((code & 0x38000) == 0x18000)
			offset = 0x100; //Debug
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		cpuRegs.CP0.n.EPC          = cpuRegs.pc;
		if (bd)
		{
			cpuRegs.CP0.n.EPC   -= 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
			cpuRegs.CP0.n.Cause &= ~0x80000000;
	}
	else
		offset = 0x180; //Override the cause

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;
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
	GPR_reg64 result;
	int32_t x      = cpuRegs.GPR.r[_Rs_].SD[0];
	int32_t y      = _Imm_;
	result.SD[0]   = (int64_t)x + y;
	if((result.UL[0]>>31) != (result.UL[1] & 1))
		cpuException(0x30, cpuRegs.branch);
	else if (_Rt_)
		cpuRegs.GPR.r[_Rt_].SD[0] = result.SD[0];
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
	int64_t x      = cpuRegs.GPR.r[_Rs_].SD[0];
	int64_t y      = _Imm_;
	int64_t result = x + y;
	if( ((~(x ^ y)) & (x ^ result)) < 0 )
		cpuException(0x30, cpuRegs.branch);		// fixme: is 0x30 right for overflow??
	else if (_Rt_)
		cpuRegs.GPR.r[_Rt_].SD[0] = 0;
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
void ADD(void)
{
	GPR_reg64 result;
	int32_t x      = cpuRegs.GPR.r[_Rs_].SD[0];
	int32_t y      = cpuRegs.GPR.r[_Rt_].SD[0];
	result.SD[0]   = (int64_t)x + y;
	if((result.UL[0]>>31) != (result.UL[1] & 1))
		cpuException(0x30, cpuRegs.branch);
	else if (_Rd_)
		cpuRegs.GPR.r[_Rd_].SD[0] = result.SD[0];
}

void DADD(void)
{
	int64_t x      = cpuRegs.GPR.r[_Rs_].SD[0];
	int64_t y      = cpuRegs.GPR.r[_Rt_].SD[0];
	int64_t result = x + y;
	if( ((~(x ^ y)) & (x ^ result)) < 0 )
		cpuException(0x30, cpuRegs.branch);		// fixme: is 0x30 right for overflow??
	else if (_Rd_)
		cpuRegs.GPR.r[_Rd_].SD[0] = 0;
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void SUB(void)
{
	GPR_reg64 result;
	int32_t x      = cpuRegs.GPR.r[_Rs_].SD[0];
	int32_t y      = -cpuRegs.GPR.r[_Rt_].SD[0];
	result.SD[0]   = (int64_t)x + y;
	if((result.UL[0]>>31) != (result.UL[1] & 1))
		cpuException(0x30, cpuRegs.branch);
	else if (_Rd_)
		cpuRegs.GPR.r[_Rd_].SD[0] = result.SD[0];
}

// Rd = Rs - Rt		(Exception on Integer Overflow)
void DSUB(void)
{
	int64_t x      =  cpuRegs.GPR.r[_Rs_].SD[0];
	int64_t y      = -cpuRegs.GPR.r[_Rt_].SD[0];
	int64_t result = x + y;
	if( ((~(x ^ y)) & (x ^ result)) < 0 )
		cpuException(0x30, cpuRegs.branch);		// fixme: is 0x30 right for overflow??
	else if (_Rd_)
		cpuRegs.GPR.r[_Rd_].SD[0] = 0;
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
	if (_Rt_) cpuRegs.GPR.r[_Rt_].SD[0] = vtlb_memRead8(addr);
}

void LBU(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = vtlb_memRead8(addr);
}

void LH(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	if (unlikely(addr & 1))
		Cpu->CancelInstruction();
	if (_Rt_) cpuRegs.GPR.r[_Rt_].SD[0] = vtlb_memRead16(addr);
}

void LHU(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	if (unlikely(addr & 1))
		Cpu->CancelInstruction();
	if (_Rt_) cpuRegs.GPR.r[_Rt_].UD[0] = vtlb_memRead16(addr);
}

void LW(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	if (unlikely(addr & 3))
		Cpu->CancelInstruction();
	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].SD[0] = (int32_t)vtlb_memRead32(addr);
}

void LWU(void)
{
	uint32_t addr = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
	if (unlikely(addr & 3))
		Cpu->CancelInstruction();
	if (_Rt_)
		cpuRegs.GPR.r[_Rt_].UD[0] = vtlb_memRead32(addr);
}

void LWL(void)
{
	static const uint32_t LWL_MASK[4] = { 0xffffff, 0x0000ffff, 0x000000ff, 0x00000000 };
	static const uint8_t LWL_SHIFT[4] = { 24, 16, 8, 0 };
	/* ensure the compiler does correct sign extension into 64 bits by using int32_t */
	if (_Rt_)
	{
		int32_t addr              = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
		uint32_t shift            = addr & 3;
		cpuRegs.GPR.r[_Rt_].SD[0] =	(int32_t)((cpuRegs.GPR.r[_Rt_].UL[0] & LWL_MASK[shift])
				              | (vtlb_memRead32(addr & ~3) << LWL_SHIFT[shift]));
	}

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

	if (_Rt_)
	{
		int32_t addr   = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
		uint32_t shift = addr & 3;
		uint32_t mem   = vtlb_memRead32(addr & ~3);
		// Use unsigned math here, and conditionally sign extend below, when needed.
		mem = (cpuRegs.GPR.r[_Rt_].UL[0] & LWR_MASK[shift]) | (mem >> LWR_SHIFT[shift]);

		// This special case requires sign extension into the full 64 bit dest.
		if (shift == 0)
			cpuRegs.GPR.r[_Rt_].SD[0] =	(int32_t)mem;
		else
			// This case sets the lower 32 bits of the target register.  Upper
			// 32 bits are always preserved.
			cpuRegs.GPR.r[_Rt_].UL[0] =	mem;
	}

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
	if(_Rt_ )
	{
		uint32_t addr        = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
		uint32_t shift       = addr & 7;
		cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDL_MASK[shift]) |
			(vtlb_memRead64(addr & ~7) << LDL_SHIFT[shift]);
	}
}

void LDR(void)
{
	static const u64 LDR_MASK[8] =
	{	0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
		0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL
	};
	static const u8 LDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	if (_Rt_)
	{
		uint32_t addr        = cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_;
		uint32_t shift       = addr & 7;
		cpuRegs.GPR.r[_Rt_].UD[0] =	(cpuRegs.GPR.r[_Rt_].UD[0] & LDR_MASK[shift]) |
			(vtlb_memRead64(addr & ~7) >> LDR_SHIFT[shift]);
	}
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
	vtlb_memWrite32( addr & ~3,
		  (cpuRegs.GPR.r[_Rt_].UL[0] >> SWL_SHIFT[shift])
		| (vtlb_memRead32( addr & ~3 ) & SWL_MASK[shift])
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
	vtlb_memWrite32( addr & ~3,
		(cpuRegs.GPR.r[_Rt_].UL[0] << SWR_SHIFT[shift]) |
		(vtlb_memRead32(addr & ~3) & SWR_MASK[shift])
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

void cpuReset()
{
	GetVmMemory().Reset();

	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&fpuRegs, 0, sizeof(fpuRegs));
	memset(&tlb, 0, sizeof(tlb));

	cpuRegs.pc				= 0xbfc00000; //set pc reg to stack
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004; //0x10900000 <-- wrong; // COP0 enabled | BEV = 1 | TS = 1
	cpuRegs.CP0.n.PRid		= 0x00002e20; // PRevID = Revision ID, same as R5900
	fpuRegs.fprc[0]			= 0x00002e30; // fpu Revision..
	fpuRegs.fprc[31]		= 0x01000001; // fpu Status/Control

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();		// lazy, no good header for it yet.
	Deci2Reset();

	g_SkipBiosHack = EmuConfig.UseBOOT2Injection;
	AllowParams1 = !g_SkipBiosHack;
	AllowParams2 = !g_SkipBiosHack;

	ElfCRC = 0;
	DiscSerial.clear();
	ElfEntry = -1;
	g_GameStarted = false;
	g_GameLoading = false;

	// FIXME: LastELF should be reset on media changes as well as on CPU resets, in
	// the very unlikely case that a user swaps to another media source that "looks"
	// the same (identical ELF names) but is actually different (devs actually could
	// run into this while testing minor binary hacked changes to ISO images, which
	// is why I found out about this) --air
	LastELF.clear();

	g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

// sets a branch test to occur some time from an arbitrary starting point.
__fi void cpuSetNextEvent( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
		cpuRegs.nextEventCycle = startCycle + delta;
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
	return std::max(1, cycles);
}

// tests the CPU cycle against the given start and delta values.
// Returns true if the delta time has passed.
__fi int cpuTestCycle( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.
	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

// tells the EE to run the branch test the next time it gets a chance.
__fi void cpuSetEvent(void)
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
}

__fi void cpuClearInt(uint i)
{
	cpuRegs.interrupt &= ~(1 << i);
	cpuRegs.dmastall &= ~(1 << i);
}

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(!g_GameStarted || CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
		cpuClearInt( n );
		callback();
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

static void MTVUInterrupt(void) { vuRegs[0].VI[REG_VPU_STAT].UL &= ~0xFF00; }

// [TODO] move this function to Dmac.cpp, and remove most of the DMAC-related headers from
// being included into R5900.cpp.
static __fi bool _cpuTestInterrupts(void)
{
	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
		return false;

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		/* These are 'pcsx2 interrupts', they handle asynchronous stuff
		   that depends on the cycle timings */
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);
		// Profile-guided Optimization (sorta)
		// The following ints are rarely called.  Encasing them in a conditional
		// as follows helps speed up most games.

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, IPUProcessInterrupt);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	return false;
}

static __fi void _cpuTestTIMR(void)
{
	cpuRegs.CP0.n.Count += cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
	cpuRegs.lastCOP0Cycle = cpuRegs.cycle;

	// fixme: this looks like a hack to make up for the fact that the TIMR
	// doesn't yet have a proper mechanism for setting itself up on a nextEventCycle.
	// A proper fix would schedule the TIMR to trigger at a specific cycle anytime
	// the Count or Compare registers are modified.

	if (       (cpuRegs.CP0.n.Status.val & 0x8000)
		&& (cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare)
		&& (cpuRegs.CP0.n.Count  < cpuRegs.CP0.n.Compare+1000))
		cpuException(0x808000, cpuRegs.branch);
}

// Checks the COP0.Status for exception enablings.
// Exception handling for certain modes is *not* currently supported, this function filters
// them out.  Exceptions while the exception handler is active (EIE), or exceptions of any
// level other than 0 are ignored here.

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt); //Choose either INTC or DMAC, depending on what called it

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

// Shared portion of the branch test, called from both the Interpreter
// and the recompiler.  (moved here to help alleviate redundant code)
__fi void _cpuEventTest_Shared(void)
{
	eeEventTestIsActive = true;
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
	cpuRegs.lastEventCycle = cpuRegs.cycle;
	// ---- INTC / DMAC (CPU-level Exceptions) -----------------
	// Done first because exceptions raised during event tests need to be postponed a few
	// cycles (fixes Grandia II [PAL], which does a spin loop on a vsync and expects to
	// be able to read the value before the exception handler clears it).

	uint mask = intcInterrupt() | dmacInterrupt();
	if (cpuIntsEnabled(mask))
		cpuException(mask, cpuRegs.branch);


	// ---- Counters -------------
	// Important: the vsync counter must be the first to be checked.  It includes emulation
	// escape/suspend hooks, and it's really a good idea to suspend/resume emulation before
	// doing any actual meaningful branchtest logic.

	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
	{
		rcntUpdate();
		// Perfs are updated when read by games (COP0's MFC0/MTC0 instructions), so we need
		// only update them at semi-regular intervals to keep cpuRegs.cycle from wrapping
		// around twice on us btween updates.  Hence this function is called from the cpu's
		// Counters update.
		COP0_UpdatePCCR();
	}

	_cpuTestTIMR();

	// ---- Interrupts -------------
	// These are basically just DMAC-related events, which also piggy-back the same bits as
	// the PS2's own DMA channel IRQs and IRQ Masks.

	if (cpuRegs.interrupt)
	{
		// This is a BIOS hack because the coding in the BIOS is terrible but the bug is masked by Data Cache
		// where a DMA buffer is overwritten without waiting for the transfer to end, which causes the fonts to get all messed up
		// so to fix it, we run all the DMA's instantly when in the BIOS.
		// Only use the lower 17 bits of the cpuRegs.interrupt as the upper bits are for VU0/1 sync which can't be done in a tight loop
		if ((!g_GameStarted || CHECK_INSTANTDMAHACK) && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
		{
			while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
				;
		}
		else
			_cpuTestInterrupts();
	}


	// ---- IOP -------------
	// * It's important to run a iopEventTest before calling ExecuteBlock. This
	//   is because the IOP does not always perform branch tests before returning
	//   (during the prev branch) and also so it can act on the state the EE has
	//   given it before executing any code.
	//
	// * The IOP cannot always be run.  If we run IOP code every time through the
	//   cpuEventTest, the IOP generally starts to run way ahead of the EE.

	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{
		EEsCycle = psxCpu->ExecuteBlock(EEsCycle);

		iopEventAction = false;
	}

	iopEventTest();

	// ---- VU Sync -------------
	// We're in a EventTest.  All dynarec registers are flushed
	// so there is no need to freeze registers here.
	CpuVU0->ExecuteBlock();
	CpuVU1->ExecuteBlock();

	// ---- Schedule Next Event Test --------------

	// EE's running way ahead of the IOP still, so we should branch quickly to give the
	// IOP extra timeslices in short order.
	const int nextIopEventDeta = ((psxRegs.iopNextEventCycle - psxRegs.cycle) * 8);
	// 8 or more cycles behind and there's an event scheduled
	if (EEsCycle >= nextIopEventDeta)
		cpuSetNextEvent(cpuRegs.cycle, 48);
	else
	{
		// Otherwise IOP is caught up/not doing anything so we can wait for the next event.
		cpuSetNextEvent(cpuRegs.cycle, ((psxRegs.iopNextEventCycle - psxRegs.cycle) * 8) - EEsCycle);
	}

	// Apply vsync and other counter nextDeltaCycles
	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);

	eeEventTestIsActive = false;
}

__ri void cpuTestINTCInts(void)
{
	// Check the COP0's Status register for general interrupt disables, and the 0x400
	// bit (which is INTC master toggle).
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEvent(cpuRegs.cycle, 4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts(void)
{
	// Check the COP0's Status register for general interrupt disables, and the 0x800
	// bit (which is the DMAC master toggle).
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEvent(cpuRegs.cycle, 4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	// If it's retunning too quick, just rerun the DMA, there's no point in running the EE for < 4 cycles.
	// This causes a huge uplift in performance for ONI FMV's.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}

	// EE events happen 8 cycles in the future instead of whatever was requested.
	// This can be used on games with PATH3 masking issues for example, or when
	// some FMV look bad.
	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;

	// Interrupt is happening soon: make sure both EE and IOP are aware.

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{
		// If running in the IOP, force it to break immediately into the EE.
		// the EE's branch test is due to run.

		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEvent(cpuRegs.cycle, cpuRegs.eCycle[n]);
}

// Called from recompilers; define is mandatory.
void eeGameStarting(void)
{
	if (!g_GameStarted)
	{
		g_GameStarted = true;
		g_GameLoading = false;

		// GameStartingInThread may issue a reset of the cpu and/or recompilers.  Check for and
		// handle such things here:
		VMManager::Internal::GameStartingOnCPUThread();
		if (VMManager::Internal::IsExecutionInterrupted())
			Cpu->ExitExecution();
	}
}

// Count arguments, save their starting locations, and replace the space separators with null terminators so they're separate strings
static int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc         = 0;
	bool wasSpace    = true; // status of last char. scanned
	size_t args_len  = strlen((char *)PSM(arg_block));
	for (int i = 0; i < (int)args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break; // should never reach this

		bool isSpace = (curchar == ' ');
		if (isSpace)
			memset(PSM(arg_block + i), 0, 1);
		else if (wasSpace) // then we're at a new arg
		{
			if (argc < KMAXARGS)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
				break;
		}
		wasSpace = isSpace;
	}
	return argc;
}

// Called from recompilers; define is mandatory.
void eeloadHook(void)
{
	std::string discelf;
	std::string elfname;
	const std::string& elf_override(VMManager::Internal::GetElfOverride());

	if (!elf_override.empty())
		cdvdReloadElfInfo(StringUtil::StdStringFromFormat("host:%s", elf_override.c_str()));
	else
		cdvdReloadElfInfo();

	int disctype = GetPS2ElfName(discelf);
	int argc     = cpuRegs.GPR.n.a0.SD[0];
	if (argc) // calls to EELOAD *after* the first one during the startup process will come here
	{
		if (argc > 1)
			elfname = (char*)PSM(vtlb_memRead32(cpuRegs.GPR.n.a1.UD[0] + 4)); // argv[1] in OSDSYS's invocation "EELOAD <game ELF>"

		// This code fires if the user chooses "full boot". First the Sony Computer Entertainment screen appears. This is the result
		// of an EELOAD call that does not want to accept launch arguments (but we patch it to do so in eeloadHook2() in fast boot
		// mode). Then EELOAD is called with the argument "rom0:PS2LOGO". At this point, we do not need any additional tricks
		// because EELOAD is now ready to accept launch arguments. So in full-boot mode, we simply wait for PS2LOGO to be called,
		// then we add the desired launch arguments. PS2LOGO passes those on to the game itself as it calls EELOAD a third time.
		if (!EmuConfig.CurrentGameArgs.empty() && !strcmp(elfname.c_str(), "rom0:PS2LOGO"))
		{
			// Join all arguments by space characters so they can be processed as one string by ParseArgumentString(), then add the
			// user's launch arguments onto the end
			u32    arg_ptr = 0;
			size_t arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = vtlb_memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				memset(PSM(arg_ptr + arg_len), 0x20, 1);
			}
			strcpy((char *)PSM(arg_ptr + arg_len + 1), EmuConfig.CurrentGameArgs.c_str());
			u32 first_arg_ptr = vtlb_memRead32(cpuRegs.GPR.n.a1.UD[0]);
			argc = ParseArgumentString(first_arg_ptr);

			// Write pointer to next slot in $a1
			for (int a = 0; a < argc; a++)
				vtlb_memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
		}
		// else it's presumed that the invocation is "EELOAD <game ELF> <<launch args>>", coming from PS2LOGO, and we needn't do
		// anything more
	}

	// If "fast boot" was chosen, then on EELOAD's first call we won't yet know what the game's ELF is. Find the name and write it
	// into EELOAD's memory.
	if (g_SkipBiosHack && elfname.empty())
	{
		std::string elftoload;
		if (!elf_override.empty())
			elftoload = StringUtil::StdStringFromFormat("host:%s", elf_override.c_str());
		else
		{
			if (disctype == 2)
				elftoload = discelf;
			else
				g_SkipBiosHack = false; // We're not fast booting, so disable it (Fixes some weirdness with the BIOS)
		}

		// When fast-booting, we insert the game's ELF name into EELOAD so that the game is called instead of the default call of
		// "rom0:OSDSYS"; any launch arguments supplied by the user will be inserted into EELOAD later by eeloadHook2()
		if (!elftoload.empty())
		{
			// Find and save location of default/fallback call "rom0:OSDSYS"; to be used later by eeloadHook2()
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8) // strings are 64-bit aligned
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					// Overwrite OSDSYS with game's ELF name
					strcpy((char*)PSM(g_osdsys_str), elftoload.c_str());
					g_GameLoading = true;
					return;
				}
			}
		}
	}

	if (!g_GameStarted && ((disctype == 2 && elfname == discelf) || disctype == 1))
		g_GameLoading = true;
}

// Called from recompilers; define is mandatory.
// Only called if g_SkipBiosHack is true
void eeloadHook2(void)
{
	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
		return;

	// Add args string after game's ELF name that was written over "rom0:OSDSYS" by eeloadHook(). In between the ELF name and args
	// string we insert a space character so that ParseArgumentString() has one continuous string to process.
	size_t game_len = strlen((char *)PSM(g_osdsys_str));
	memset(PSM(g_osdsys_str + game_len), 0x20, 1);
	strcpy((char *)PSM(g_osdsys_str + game_len + 1), EmuConfig.CurrentGameArgs.c_str());
	int argc = ParseArgumentString(g_osdsys_str);

	// Back up 4 bytes from start of args block for every arg + 4 bytes for start of argv pointer block, write pointers
	uintptr_t block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
		vtlb_memWrite32(block_start + (a * 4), g_argPtrs[a]);

	// Save argc and argv as incoming arguments for EELOAD function which calls ExecPS2()
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}
