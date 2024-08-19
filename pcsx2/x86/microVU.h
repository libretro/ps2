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

#include <stdint.h>
#include <deque>
#include <algorithm>
#include <cstring> /* memset/memcpy */
#include <memory>

#include "Common.h"
#include "VU.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"
#include "VirtualMemory.h"
#include "x86emitter.h"

using namespace x86Emitter;

typedef xRegisterSSE xmm;
typedef xRegister32 x32;

struct microVU;

//------------------------------------------------------------------
// Global Variables
//------------------------------------------------------------------

struct mVU_Globals
{
	uint32_t   absclip[4], signbit[4], minvals[4], maxvals[4];
	uint32_t   one[4];
	uint32_t   Pi4[4];
	uint32_t   T1[4], T2[4], T3[4], T4[4], T5[4], T6[4], T7[4], T8[4];
	uint32_t   S2[4], S3[4], S4[4], S5[4];
	uint32_t   E1[4], E2[4], E3[4], E4[4], E5[4], E6[4];
	float FTOI_4[4], FTOI_12[4], FTOI_15[4];
	float ITOF_4[4], ITOF_12[4], ITOF_15[4];
};

#define __four(val) { val, val, val, val }
alignas(32) static const mVU_Globals mVUglob = {
	__four(0x7fffffff),       // absclip
	__four(0x80000000),       // signbit
	__four(0xff7fffff),       // minvals
	__four(0x7f7fffff),       // maxvals
	__four(0x3f800000),       // ONE!
	__four(0x3f490fdb),       // PI4!
	__four(0x3f7ffff5),       // T1
	__four(0xbeaaa61c),       // T5
	__four(0x3e4c40a6),       // T2
	__four(0xbe0e6c63),       // T3
	__four(0x3dc577df),       // T4
	__four(0xbd6501c4),       // T6
	__four(0x3cb31652),       // T7
	__four(0xbb84d7e7),       // T8
	__four(0xbe2aaaa4),       // S2
	__four(0x3c08873e),       // S3
	__four(0xb94fb21f),       // S4
	__four(0x362e9c14),       // S5
	__four(0x3e7fffa8),       // E1
	__four(0x3d0007f4),       // E2
	__four(0x3b29d3ff),       // E3
	__four(0x3933e553),       // E4
	__four(0x36b63510),       // E5
	__four(0x353961ac),       // E6
	__four(16.0),             // FTOI_4
	__four(4096.0),           // FTOI_12
	__four(32768.0),          // FTOI_15
	__four(0.0625f),          // ITOF_4
	__four(0.000244140625),   // ITOF_12
	__four(0.000030517578125) // ITOF_15
};

static const uint _Ibit_ = 1 << 31;
static const uint _Ebit_ = 1 << 30;
static const uint _Mbit_ = 1 << 29;
static const uint _Dbit_ = 1 << 28;
static const uint _Tbit_ = 1 << 27;

static const uint divI = 0x1040000;
static const uint divD = 0x2080000;

//------------------------------------------------------------------
// Helper Macros
//------------------------------------------------------------------

#define _Ft_ ((mVU.code >> 16) & 0x1F) // The ft part of the instruction register
#define _Fs_ ((mVU.code >> 11) & 0x1F) // The fs part of the instruction register
#define _Fd_ ((mVU.code >>  6) & 0x1F) // The fd part of the instruction register

#define _It_ ((mVU.code >> 16) & 0xF)  // The it part of the instruction register
#define _Is_ ((mVU.code >> 11) & 0xF)  // The is part of the instruction register
#define _Id_ ((mVU.code >>  6) & 0xF)  // The id part of the instruction register

#define _X ((mVU.code >> 24) & 0x1)
#define _Y ((mVU.code >> 23) & 0x1)
#define _Z ((mVU.code >> 22) & 0x1)
#define _W ((mVU.code >> 21) & 0x1)

#define _cX ((cpuRegs.code >> 24) & 0x1)
#define _cY ((cpuRegs.code >> 23) & 0x1)
#define _cZ ((cpuRegs.code >> 22) & 0x1)
#define _cW ((cpuRegs.code >> 21) & 0x1)

#define _X_Y_Z_W   (((mVU.code >> 21) & 0xF))
#define _cX_Y_Z_W   (((cpuRegs.code >> 21) & 0xF))
#define _cXYZW_SS  (_cX + _cY + _cZ + _cW == 1)
#define _cXYZW_SS2  (_cXYZW_SS && (_cX_Y_Z_W != 8))

#define _XYZW_SS   (_X + _Y + _Z + _W == 1)
#define _XYZW_SS2  (_XYZW_SS && (_X_Y_Z_W != 8))
#define _XYZW_PS   (_X_Y_Z_W == 0xf)
#define _XYZWss(x) ((x == 8) || (x == 4) || (x == 2) || (x == 1))

#define _bc_   (mVU.code & 0x3)
#define _bc_x ((mVU.code & 0x3) == 0)
#define _bc_y ((mVU.code & 0x3) == 1)
#define _bc_z ((mVU.code & 0x3) == 2)
#define _bc_w ((mVU.code & 0x3) == 3)

#define _Fsf_ ((mVU.code >> 21) & 0x03)
#define _Ftf_ ((mVU.code >> 23) & 0x03)

#define _Imm5_  ((int16_t) (((mVU.code & 0x400) ? 0xfff0 : 0) | ((mVU.code >> 6) & 0xf)))
#define _Imm11_ ((int32_t)  ((mVU.code & 0x400) ? (0xfffffc00 |  (mVU.code & 0x3ff)) : (mVU.code & 0x3ff)))
#define _Imm12_ ((uint32_t)((((mVU.code >> 21) & 0x1) << 11)   |  (mVU.code & 0x7ff)))
#define _Imm15_ ((uint32_t) (((mVU.code >> 10) & 0x7800)       |  (mVU.code & 0x7ff)))
#define _Imm24_ ((uint32_t)   (mVU.code & 0xffffff))

#define isCOP2      (mVU.cop2 != 0)
#define isVU1       (mVU.index != 0)
#define isVU0       (mVU.index == 0)
#define getIndex    (isVU1 ? 1 : 0)
#define getVUmem(x) (((isVU1) ? (x & 0x3ff) : ((x >= 0x400) ? (x & 0x43f) : (x & 0xff))) * 16)
#define offsetSS    ((_X) ? (0) : ((_Y) ? (4) : ((_Z) ? 8 : 12)))
#define offsetReg   ((_X) ? (0) : ((_Y) ? (1) : ((_Z) ? 2 :  3)))

#define xmmT1  xmm0 // Used for regAlloc
#define xmmT2  xmm1 // Used for regAlloc
#define xmmT3  xmm2 // Used for regAlloc
#define xmmT4  xmm3 // Used for regAlloc
#define xmmT5  xmm4 // Used for regAlloc
#define xmmT6  xmm5 // Used for regAlloc
#define xmmT7  xmm6 // Used for regAlloc
#define xmmPQ  xmm15 // Holds the Value and Backup Values of P and Q regs

#define gprT1  eax // eax - Temp Reg
#define gprT2  ecx // ecx - Temp Reg
#define gprT1q rax // eax - Temp Reg
#define gprT2q rcx // ecx - Temp Reg
#define gprT1b ax  // Low 16-bit of gprT1 (eax)
#define gprT2b cx  // Low 16-bit of gprT2 (ecx)

#define gprF0  ebx // Status Flag 0
#define gprF1 r12d // Status Flag 1
#define gprF2 r13d // Status Flag 2
#define gprF3 r14d // Status Flag 3

// Function Params
#define mP microVU& mVU, int recPass
#define mV microVU& mVU
#define mF int recPass
#define mX mVU, recPass

typedef void Fntype_mVUrecInst(microVU& mVU, int recPass);
typedef Fntype_mVUrecInst* Fnptr_mVUrecInst;

// Function/Template Stuff
#define mVUx (vuIndex ? microVU1 : microVU0)
#define mVUop(opName) static void opName(mP)
#define _mVUt template <int vuIndex>

// Define Passes
#define pass1 if (recPass == 0) // Analyze
#define pass2 if (recPass == 1) // Recompile
#define pass4 if (recPass == 3) // Flag stuff

// Upper Opcode Cases
#define opCase1 if (opCase == 1) // Normal Opcodes
#define opCase2 if (opCase == 2) // BC Opcodes
#define opCase3 if (opCase == 3) // I  Opcodes
#define opCase4 if (opCase == 4) // Q  Opcodes

//------------------------------------------------------------------

// Misc Macros...
#define mVUcurProg   mVU.prog.cur[0]
#define mVUblocks    mVU.prog.cur->block
#define mVUir        mVU.prog.IRinfo
#define mVUbranch    mVU.prog.IRinfo.branch
#define mVUcycles    mVU.prog.IRinfo.cycles
#define mVUcount     mVU.prog.IRinfo.count
#define mVUpBlock    mVU.prog.IRinfo.pBlock
#define mVUblock     mVU.prog.IRinfo.block
#define mVUregs      mVU.prog.IRinfo.block.pState
#define mVUregsTemp  mVU.prog.IRinfo.regsTemp
#define iPC          mVU.prog.IRinfo.curPC
#define mVUsFlagHack mVU.prog.IRinfo.sFlagHack
#define mVUconstReg  mVU.prog.IRinfo.constReg
#define mVUstartPC   mVU.prog.IRinfo.startPC
#define mVUinfo      mVU.prog.IRinfo.info[iPC / 2]
#define mVUstall     mVUinfo.stall
#define mVUup        mVUinfo.uOp
#define mVUlow       mVUinfo.lOp
#define sFLAG        mVUinfo.sFlag
#define mFLAG        mVUinfo.mFlag
#define cFLAG        mVUinfo.cFlag
#define mVUrange     (mVUcurProg.ranges[0])[0]
#define isEvilBlock  (mVUpBlock->pState.blockType == 2)
#define isBadOrEvil  (mVUlow.badBranch || mVUlow.evilBranch)
#define isConditional (mVUlow.branch > 2 && mVUlow.branch < 9)
#define xPC          ((iPC / 2) * 8)
#define curI         ((uint32_t*)vuRegs[mVU.index].Micro)[iPC] //mVUcurProg.data[iPC]
#define setCode()    { mVU.code = curI; }
#define bSaveAddr    (((xPC + 16) & (mVU.microMemSize-8)) / 8)
#define shufflePQ    (((mVU.p) ? 0xb0 : 0xe0) | ((mVU.q) ? 0x01 : 0x04))
#define Rmem         &vuRegs[mVU.index].VI[REG_R].UL
#define aWrap(x, m)  ((x > m) ? 0 : x)
#define shuffleSS(x) ((x == 1) ? (0x27) : ((x == 2) ? (0xc6) : ((x == 4) ? (0xe1) : (0xe4))))
#define clampE       CHECK_VU_EXTRA_OVERFLOW(mVU.index)
#define islowerOP    ((iPC & 1) == 0)

#define blockCreate(addr) \
	{ \
		if (!mVUblocks[addr]) \
			mVUblocks[addr] = new microBlockManager(); \
	}

// Fetches the PC and instruction opcode relative to the current PC.  Used to rewind and
// fast-forward the IR state while calculating VU pipeline conditions (branches, writebacks, etc)
#define incPC(x)  { iPC = ((iPC + (x)) & mVU.progMemMask); mVU.code = curI; }
#define incPC2(x) { iPC = ((iPC + (x)) & mVU.progMemMask); }

// Flag Info (Set if next-block's first 4 ops will read current-block's flags)
#define __Status (mVUregs.needExactMatch & 1)
#define __Mac    (mVUregs.needExactMatch & 2)
#define __Clip   (mVUregs.needExactMatch & 4)

//------------------------------------------------------------------
// Optimization / Debug Options
//------------------------------------------------------------------

// Reg Alloc
static constexpr bool doRegAlloc = true; // Set to false to flush every 32bit Instruction
// This turns off reg alloc for the most part, but reg alloc will still
// be done within instructions... Also on doSwapOp() regAlloc is needed between
// Lower and Upper instructions, so in this case it flushes after the full
// 64bit instruction (lower and upper)

// No Flag Optimizations
static constexpr bool noFlagOpts = false; // Set to true to disable all flag setting optimizations
// Note: The flag optimizations this disables should all be harmless, so
// this option is mainly just for debugging... it effectively forces mVU
// to always update Mac and Status Flags (both sticky and non-sticky) whenever
// an Upper Instruction updates them. It also always transfers the 4 possible
// flag instances between blocks...

// Multiple Flag Instances
static constexpr bool doSFlagInsts = true; // Set to true to enable multiple status flag instances
static constexpr bool doMFlagInsts = true; // Set to true to enable multiple mac    flag instances
static constexpr bool doCFlagInsts = true; // Set to true to enable multiple clip   flag instances
// This is the correct behavior of the VU's. Due to the pipeline of the VU's
// there can be up to 4 different instances of values to keep track of
// for the 3 different types of flags: Status, Mac, Clip flags.
// Setting one of these to 0 acts as if there is only 1 instance of the
// corresponding flag, which may be useful when debugging flag pipeline bugs.

// Branch in Branch Delay Slots
static constexpr bool doBranchInDelaySlot = true; // Set to true to enable evil-branches
// This attempts to emulate the correct behavior for branches in branch delay
// slots. It is evil that games do this, and handling the different possible
// cases is tricky and bug prone. If this option is disabled then the second
// branch is treated as a NOP and effectively ignored.

// Constant Propagation
static constexpr bool doConstProp = false; // Set to true to turn on vi15 const propagation
// Enables Constant Propagation for Jumps based on vi15 'link-register'
// allowing us to know many indirect jump target addresses.
// Makes GoW a lot slower due to extra recompilation time and extra code-gen!

// Indirect Jump Caching
static constexpr bool doJumpCaching = true; // Set to true to enable jump caching
// Indirect jumps (JR/JALR) will remember the entry points to their previously
// jumped-to addresses. This allows us to skip the microBlockManager::search()
// routine that is performed every indirect jump in order to find a block within a
// program that matches the correct pipeline state.

// Indirect Jumps are part of same cached microProgram
static constexpr bool doJumpAsSameProgram = false; // Set to true to treat jumps as same program
// Enabling this treats indirect jumps (JR/JALR) as part of the same microProgram
// when determining the valid ranges for the microProgram cache. Disabling this
// counts indirect jumps as separate cached microPrograms which generally leads
// to more microPrograms being cached, but the programs created are smaller and
// the overall cache usage ends up being more optimal; it can also help prevent
// constant recompilation problems in certain games.
// Note: You MUST disable doJumpCaching if you enable this option.

// Handling of D-Bit in Micro Programs
static constexpr bool doDBitHandling = false;
// This flag shouldn't be enabled in released versions of games. Any games which
// need this method of pausing the VU should be using the T-Bit instead, however
// this could prove useful for VU debugging.

// Whole program comparison on search
static constexpr bool doWholeProgCompare = false;
// This shouldn't be needed and could inflate program generation.
// Compares the entire VU memory with the stored micro program's memory, regardless of if it's used.
// Generally slower but may be useful for debugging.

//------------------------------------------------------------------
// Speed Hacks (can cause infinite loops, SPS, Black Screens, etc...)
//------------------------------------------------------------------

// Status Flag Speed Hack
#define CHECK_VU_FLAGHACK (EmuConfig.Speedhacks.vuFlagHack)
// This hack only updates the Status Flag on blocks that will read it.
// Most blocks do not read status flags, so this is a big speedup.

extern void mVUmergeRegs(const xmm& dest, const xmm& src, int xyzw, bool modXYZW = false);
extern void mVUsaveReg(const xmm& reg, xAddressVoid ptr, int xyzw, bool modXYZW);
extern void mVUloadReg(const xmm& reg, xAddressVoid ptr, int xyzw);

struct regCycleInfo
{
	uint8_t x : 4;
	uint8_t y : 4;
	uint8_t z : 4;
	uint8_t w : 4;
};

// microRegInfo is carefully ordered for faster compares.  The "important" information is
// housed in a union that is accessed via 'quick32' so that several uint8_t fields can be compared
// using a pair of 32-bit equalities.
// vi15 is only used if microVU const-prop is enabled (it is *not* by default).  When constprop
// is disabled the vi15 field acts as additional padding that is required for 16 byte alignment
// needed by the xmm compare.
union alignas(16) microRegInfo
{
	struct
	{
		union
		{
			struct
			{
				uint8_t needExactMatch; // If set, block needs an exact match of pipeline state
				uint8_t flagInfo;       // xC * 2 | xM * 2 | xS * 2 | 0 * 1 | fullFlag Valid * 1
				uint8_t q;
				uint8_t p;
				uint8_t xgkick;
				uint8_t viBackUp;       // VI reg number that was written to on branch-delay slot
				uint8_t blockType;      // 0 = Normal; 1,2 = Compile one instruction (E-bit/Branch Ending)
				uint8_t r;
			};
			uint64_t quick64[1];
			uint32_t quick32[2];
		};

		uint32_t xgkickcycles;
		uint8_t unused;
		uint8_t vi15v; // 'vi15' constant is valid
		uint16_t vi15; // Constant Prop Info for vi15

		struct
		{
			uint8_t VI[16];
			regCycleInfo VF[32];
		};
	};

	u128 full128[96 / sizeof(u128)];
	uint64_t  full64[96 / sizeof(uint64_t)];
	uint32_t  full32[96 / sizeof(uint32_t)];
};

struct microProgram;
struct microJumpCache
{
	microJumpCache() : prog(NULL), x86ptrStart(NULL) {}
	microProgram* prog; // Program to which the entry point below is part of
	void* x86ptrStart;  // Start of code (Entry point for block)
};

struct alignas(16) microBlock
{
	microRegInfo    pState;      // Detailed State of Pipeline
	microRegInfo    pStateEnd;   // Detailed State of Pipeline at End of Block (needed by JR/JALR opcodes)
	uint8_t*        x86ptrStart; // Start of code (Entry point for block)
	microJumpCache* jumpCache;   // Will point to an array of entry points of size [16k/8] if block ends in JR/JALR
};

struct microTempRegInfo
{
	regCycleInfo VF[2]; // Holds cycle info for Fd, VF[0] = Upper Instruction, VF[1] = Lower Instruction
	uint8_t VFreg[2];   // Index of the VF reg
	uint8_t VI;         // Holds cycle info for Id
	uint8_t VIreg;      // Index of the VI reg
	uint8_t q;          // Holds cycle info for Q reg
	uint8_t p;          // Holds cycle info for P reg
	uint8_t r;          // Holds cycle info for R reg (Will never cause stalls, but useful to know if R is modified)
	uint8_t xgkick;     // Holds the cycle info for XGkick
};

struct microVFreg
{
	uint8_t reg; // Reg Index
	uint8_t x;   // X vector read/written to?
	uint8_t y;   // Y vector read/written to?
	uint8_t z;   // Z vector read/written to?
	uint8_t w;   // W vector read/written to?
};

struct microVIreg
{
	uint8_t reg;  // Reg Index
	uint8_t used; // Reg is Used? (Read/Written)
};

struct microConstInfo
{
	uint8_t  isValid;  // Is the constant in regValue valid?
	uint32_t regValue; // Constant Value
};

struct microUpperOp
{
	bool eBit;             // Has E-bit set
	bool iBit;             // Has I-bit set
	bool mBit;             // Has M-bit set
	bool tBit;             // Has T-bit set
	bool dBit;             // Has D-bit set
	microVFreg VF_write;   // VF Vectors written to by this instruction
	microVFreg VF_read[2]; // VF Vectors read by this instruction
};

struct microLowerOp
{
	microVFreg VF_write;      // VF Vectors written to by this instruction
	microVFreg VF_read[2];    // VF Vectors read by this instruction
	microVIreg VI_write;      // VI reg written to by this instruction
	microVIreg VI_read[2];    // VI regs read by this instruction
	microConstInfo constJump; // Constant Reg Info for JR/JARL instructions
	uint32_t  branch;     // Branch Type (0 = Not a Branch, 1 = B. 2 = BAL, 3~8 = Conditional Branches, 9 = JR, 10 = JALR)
	uint32_t  kickcycles; // Number of xgkick cycles accumulated by this instruction
	bool badBranch;  // This instruction is a Branch who has another branch in its Delay Slot
	bool evilBranch; // This instruction is a Branch in a Branch Delay Slot (Instruction after badBranch)
	bool isNOP;      // This instruction is a NOP
	bool isFSSET;    // This instruction is a FSSET
	bool noWriteVF;  // Don't write back the result of a lower op to VF reg if upper op writes to same reg (or if VF = 0)
	bool backupVI;   // Backup VI reg to memory if modified before branch (branch uses old VI value unless opcode is ILW or ILWR)
	bool memReadIs;  // Read Is (VI reg) from memory (used by branches)
	bool memReadIt;  // Read If (VI reg) from memory (used by branches)
	bool readFlags;  // Current Instruction reads Status, Mac, or Clip flags
	bool isMemWrite; // Current Instruction writes to VU memory
	bool isKick;     // Op is a kick so don't count kick cycles
};

struct microFlagInst
{
	bool doFlag;      // Update Flag on this Instruction
	bool doNonSticky; // Update O,U,S,Z (non-sticky) bits on this Instruction (status flag only)
	uint8_t   write;       // Points to the instance that should be written to (s-stage write)
	uint8_t   lastWrite;   // Points to the instance that was last written to (most up-to-date flag)
	uint8_t   read;        // Points to the instance that should be read by a lower instruction (t-stage read)
};

struct microFlagCycles
{
	int xStatus[4];
	int xMac[4];
	int xClip[4];
	int cycles;
};

struct microOp
{
	uint8_t   stall;          // Info on how much current instruction stalled
	bool isBadOp;        // Cur Instruction is a bad opcode (not a legal instruction)
	bool isEOB;          // Cur Instruction is last instruction in block (End of Block)
	bool isBdelay;       // Cur Instruction in Branch Delay slot
	bool swapOps;        // Run Lower Instruction before Upper Instruction
	bool backupVF;       // Backup mVUlow.VF_write.reg, and restore it before the Upper Instruction is called
	bool doXGKICK;       // Do XGKICK transfer on this instruction
	uint32_t  XGKICKPC;       // The PC in which the XGKick has taken place, so if we break early (before it) we don run it.
	bool doDivFlag;      // Transfer Div flag to Status Flag on this instruction
	int  readQ;          // Q instance for reading
	int  writeQ;         // Q instance for writing
	int  readP;          // P instance for reading
	int  writeP;         // P instance for writing
	microFlagInst sFlag; // Status Flag Instance Info
	microFlagInst mFlag; // Mac    Flag Instance Info
	microFlagInst cFlag; // Clip   Flag Instance Info
	microUpperOp  uOp;   // Upper Op Info
	microLowerOp  lOp;   // Lower Op Info
};

template <uint32_t pSize>
struct microIR
{
	microBlock       block;           // Block/Pipeline info
	microBlock*      pBlock;          // Pointer to a block in mVUblocks
	microTempRegInfo regsTemp;        // Temp Pipeline info (used so that new pipeline info isn't conflicting between upper and lower instructions in the same cycle)
	microOp          info[pSize / 2]; // Info for Instructions in current block
	microConstInfo   constReg[16];    // Simple Const Propagation Info for VI regs within blocks
	uint8_t  branch;
	uint32_t cycles;    // Cycles for current block
	uint32_t count;     // Number of VU 64bit instructions ran (starts at 0 for each block)
	uint32_t curPC;     // Current PC
	uint32_t startPC;   // Start PC for Cur Block
	uint32_t sFlagHack; // Optimize out all Status flag updates if microProgram doesn't use Status flags
};

//------------------------------------------------------------------
// Reg Alloc
//------------------------------------------------------------------

struct microMapXMM
{
	int  VFreg;    // VF Reg Number Stored (-1 = Temp; 0 = vf0 and will not be written back; 32 = ACC; 33 = I reg)
	int  xyzw;     // xyzw to write back (0 = Don't write back anything AND cached vfReg has all vectors valid)
	int  count;    // Count of when last used
	bool isNeeded; // Is needed for current instruction
	bool isZero;   // Register was loaded from VF00 and doesn't need clamping
};

struct microMapGPR
{
	int VIreg;
	int count;
	bool isNeeded;
	bool dirty;
	bool isZeroExtended;
	bool usable;
};

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
				xMOVDZX(reg, xRegister32(i));
				if (!_XYZWss(xyzw))
					xSHUF.PS(reg, reg, 0);

				return;
			}
		}

		xMOVSSZX(reg, ptr32[&::vuRegs[index].VI[REG_I]]);
		if (!_XYZWss(xyzw))
			xSHUF.PS(reg, reg, 0);
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
			if (!RegisterSSE_IsCallerSaved(i))
				continue;

			writeBackReg(xmm(i));
			if (clearNeeded || !xmmMap[i].isNeeded)
				clearReg(i);
		}

		for (int i = 0; i < gprTotal; i++)
		{
			if (!Register_IsCallerSaved(i))
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
					xMOVSS(ptr32[&::vuRegs[index].VI[REG_I]], xmm(i));
				else if (mapX.VFreg == 32)
					mVUsaveReg(xmm(i), ptr[&::vuRegs[index].ACC], mapX.xyzw, 1);
				else
					mVUsaveReg(xmm(i), ptr[&::vuRegs[index].VI[mapX.VFreg]], mapX.xyzw, 1);
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
				xMOVSS(ptr32[&::vuRegs[index].VI[REG_I]], reg);
			else if (mapX.VFreg == 32)
				mVUsaveReg(reg, ptr[&::vuRegs[index].ACC], mapX.xyzw, true);
			else
				mVUsaveReg(reg, ptr[&::vuRegs[index].VF[mapX.VFreg]], mapX.xyzw, true);

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
								xPSHUF.D(xmmZ, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmZ, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmZ, xmmI, 3);
							else if (z != i)
								xMOVAPS(xmmZ, xmmI);

							mapI.count = counter; // Reg i was used, so update counter
						}
						else // Don't clone reg, but shuffle to adjust for SS ops
						{
							if ((vfLoadReg != vfWriteReg) || (xyzw != 0xf))
								writeBackReg(xmmI);

							if (xyzw == 4)
								xPSHUF.D(xmmI, xmmI, 1);
							else if (xyzw == 2)
								xPSHUF.D(xmmI, xmmI, 2);
							else if (xyzw == 1)
								xPSHUF.D(xmmI, xmmI, 3);
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
				xPXOR(xmmX, xmmX);
			else if (vfLoadReg == 33)
				loadIreg(xmmX, xyzw);
			else if (vfLoadReg == 32)
				mVUloadReg(xmmX, ptr[&::vuRegs[index].ACC], xyzw);
			else if (vfLoadReg >= 0)
				mVUloadReg(xmmX, ptr[&::vuRegs[index].VF[vfLoadReg]], xyzw);

			xmmMap[x].VFreg = vfWriteReg;
			xmmMap[x].xyzw  = xyzw;
		}
		else // Reg Will Not Be Modified (always load full reg for caching)
		{
			if (vfLoadReg == 33)
				loadIreg(xmmX, 0xf);
			else if (vfLoadReg == 32)
				xMOVAPS (xmmX, ptr128[&::vuRegs[index].ACC]);
			else if (vfLoadReg >= 0)
				xMOVAPS (xmmX, ptr128[&::vuRegs[index].VF[vfLoadReg]]);

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

		const uint32_t rn   = reg.Id;
		const bool dirty    = (gprMap[rn].VIreg >= 0 && gprMap[rn].dirty);
		x86regs[rn].reg     = gprMap[rn].VIreg;
		x86regs[rn].counter = gprMap[rn].count;
		x86regs[rn].mode    = dirty ? (MODE_READ | MODE_WRITE) : MODE_READ;
		x86regs[rn].needed  = gprMap[rn].isNeeded;
	}

	void writeBackReg(const xRegisterInt& reg, bool clearDirty)
	{
		microMapGPR& mapX = gprMap[reg.Id];
		if (mapX.dirty)
		{
			if (mapX.VIreg < 16)
				xMOV(ptr16[&::vuRegs[index].VI[mapX.VIreg]], xRegister16(reg));
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
				xXOR(gprX, gprX);
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
								xMOVZX(gprX, ptr16[&::vuRegs[index].VI[viWriteReg]]);
								writeVIBackup(gprX);
								backup = false;
							}

							if (zext_if_dirty)
								xMOVZX(gprX, xRegister16(i));
							else
								xMOV(gprX, xRegister32(i));
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
						xMOVZX(xRegister32(i), xRegister16(i));
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
			xMOVZX(gprX, ptr16[&::vuRegs[index].VI[viWriteReg]]);
			writeVIBackup(gprX);
			backup = false;
		}

		if (viLoadReg > 0)
			xMOVZX(gprX, ptr16[&::vuRegs[index].VI[viLoadReg]]);
		else if (viLoadReg == 0)
			xXOR(gprX, gprX);

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
					xMOVZX(gprX, ptr16[&::vuRegs[index].VI[viWriteReg]]);
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
			xXOR(xRegister32(reg), xRegister32(reg));
			return;
		}

		// TODO: Check liveness/usedness before allocating.
		// TODO: Check whether zero-extend is needed everywhere heae. Loadstores are.
		const xRegister32& srcreg = allocGPR(vi);
		if (signext)
			xMOVSX(xRegister32(reg), xRegister16(srcreg));
		else
			xMOVZX(xRegister32(reg), xRegister16(srcreg));
		clearNeeded(srcreg);
	}
};

class microBlockManager;

struct microBlockLink
{
	microBlock block;
	microBlockLink* next;
};

struct microBlockLinkRef
{
	microBlock* pBlock;
	uint64_t quick;
};

struct microRange
{
	int32_t start; /* Start PC (The opcode the block starts at) */
	int32_t end;   /* End PC   (The opcode the block ends with) */
};

#define mProgSize (0x4000 / 4)
struct microProgram
{
	uint32_t                data [mProgSize]; /* Holds a copy of the VU microProgram */
	microBlockManager* block[mProgSize / 2];  /* Array of Block Managers */
	std::deque<microRange>* ranges;           /* The ranges of the microProgram that 
						   * have already been recompiled */
	uint32_t startPC; 			  /* Start PC of this program */
	int idx;     				  /* Program index */
};

typedef std::deque<microProgram*> microProgramList;

struct microProgramQuick
{
	microBlockManager* block; // Quick reference to valid microBlockManager for current startPC
	microProgram*      prog;  // The microProgram who is the owner of 'block'
};

struct microProgManager
{
	microIR<mProgSize> IRinfo;             // IR information
	microProgramList*  prog [mProgSize/2]; // List of microPrograms indexed by startPC values
	microProgramQuick  quick[mProgSize/2]; // Quick reference to valid microPrograms for current execution
	microProgram*      cur;                // Pointer to currently running MicroProgram
	int                total;              // Total Number of valid MicroPrograms
	int                isSame;             // Current cached microProgram is Exact Same program as vuRegs[mVU.index].Micro (-1 = unknown, 0 = No, 1 = Yes)
	int                cleared;            // Micro Program is Indeterminate so must be searched for (and if no matches are found then recompile a new one)
	uint32_t                curFrame;      // Frame Counter
	uint8_t*                x86ptr;        // Pointer to program's recompilation code
	uint8_t*                x86start;      // Start of program's rec-cache
	uint8_t*                x86end;        // Limit of program's rec-cache
	microRegInfo       lpState;            // Pipeline state from where program left off 
					       // (useful for continuing execution)
};

static const uint mVUdispCacheSize = __pagesize; // Dispatcher Cache Size (in bytes)
static const uint mVUcacheSafeZone =  3; // Safe-Zone for program recompilation (in megabytes)
static const uint mVUcacheReserve = 64; // mVU0, mVU1 Reserve Cache Size (in megabytes)

struct microVU
{

	alignas(16) uint32_t statFlag[4]; // 4 instances of status flag (backup for xgkick)
	alignas(16) uint32_t macFlag [4]; // 4 instances of mac    flag (used in execution)
	alignas(16) uint32_t clipFlag[4]; // 4 instances of clip   flag (used in execution)
	alignas(16) uint32_t xmmCTemp[4];      // Backup used in mVUclamp2()
	alignas(16) uint32_t xmmBackup[16][4]; // Backup for xmm0~xmm15

	uint32_t index;        // VU Index (VU0 or VU1)
	uint32_t cop2;         // VU is in COP2 mode?  (No/Yes)
	uint32_t vuMemSize;    // VU Main  Memory Size (in bytes)
	uint32_t microMemSize; // VU Micro Memory Size (in bytes)
	uint32_t progSize;     // VU Micro Memory Size (in uint32_t's)
	uint32_t progMemMask;  // VU Micro Memory Size (in uint32_t's)
	uint32_t cacheSize;    // VU Cache Size

	microProgManager               prog;     // Micro Program Data
	std::unique_ptr<microRegAlloc> regAlloc; // Reg Alloc Class

	RecompiledCodeReserve* cache_reserve;
	uint8_t* cache;        // Dynarec Cache Start (where we will start writing the recompiled code to)
	uint8_t* dispCache;    // Dispatchers Cache (where startFunct and exitFunct are written to)
	uint8_t* startFunct;   // Function Ptr to the recompiler dispatcher (start)
	uint8_t* exitFunct;    // Function Ptr to the recompiler dispatcher (exit)
	uint8_t* startFunctXG; // Function Ptr to the recompiler dispatcher (xgkick resume)
	uint8_t* exitFunctXG;  // Function Ptr to the recompiler dispatcher (xgkick exit)
	uint8_t* compareStateF;// Function Ptr to search which compares all state.
	uint8_t* waitMTVU;     // Ptr to function to save registers/sync VU1 thread
	uint8_t* copyPLState;  // Ptr to function to copy pipeline state into microVU
	uint8_t* resumePtrXG;  // Ptr to recompiled code position to resume xgkick
	uint32_t code;         // Contains the current Instruction
	uint32_t divFlag;      // 1 instance of I/D flags
	uint32_t VIbackup;     // Holds a backup of a VI reg if modified before a branch
	uint32_t VIxgkick;     // Holds a backup of a VI reg used for xgkick-delays
	uint32_t branch;       // Holds branch compare result (IBxx) OR Holds address to Jump to (JALR/JR)
	uint32_t badBranch;    // For Branches in Branch Delay Slots, holds Address the first Branch went to + 8
	uint32_t evilBranch;   // For Branches in Branch Delay Slots, holds Address to Jump to
	uint32_t evilevilBranch;// For Branches in Branch Delay Slots (chained), holds Address to Jump to
	uint32_t p;            // Holds current P instance index
	uint32_t q;            // Holds current Q instance index
	uint32_t totalCycles;  // Total Cycles that mVU is expected to run for
	int32_t cycles;        // Cycles Counter
};

class microBlockManager
{
private:
	microBlockLink *qBlockList, *qBlockEnd; // Quick Search
	microBlockLink *fBlockList, *fBlockEnd; // Full  Search
	std::vector<microBlockLinkRef> quickLookup;

public:
	microBlockManager()
	{
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
	}
	~microBlockManager() { reset(); }
	void reset()
	{
		for (microBlockLink* linkI = qBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		for (microBlockLink* linkI = fBlockList; linkI != nullptr;)
		{
			microBlockLink* freeI = linkI;
			delete[](linkI->block.jumpCache);
			linkI->block.jumpCache = NULL;
			linkI = linkI->next;
			_aligned_free(freeI);
		}
		qBlockEnd = qBlockList = nullptr;
		fBlockEnd = fBlockList = nullptr;
		quickLookup.clear();
	};
	microBlock* add(microVU& mVU, microBlock* pBlock)
	{
		microBlock* thisBlock = search(mVU, &pBlock->pState);
		if (!thisBlock)
		{
			uint8_t fullCmp = pBlock->pState.needExactMatch;

			microBlockLink*& blockList = fullCmp ? fBlockList : qBlockList;
			microBlockLink*& blockEnd  = fullCmp ? fBlockEnd  : qBlockEnd;
			microBlockLink*  newBlock  = (microBlockLink*)_aligned_malloc(
					sizeof(microBlockLink), 32);
			newBlock->block.jumpCache  = nullptr;
			newBlock->next             = nullptr;

			if (blockEnd)
			{
				blockEnd->next = newBlock;
				blockEnd       = newBlock;
			}
			else
				blockEnd       = blockList = newBlock;

			memcpy(&newBlock->block, pBlock, sizeof(microBlock));
			thisBlock = &newBlock->block;

			quickLookup.push_back({&newBlock->block, pBlock->pState.quick64[0]});
		}
		return thisBlock;
	}
	__ri microBlock* search(microVU& mVU, microRegInfo* pState)
	{
		if (pState->needExactMatch) // Needs Detailed Search (Exact Match of Pipeline State)
		{
			microBlockLink* prevI = nullptr;
			for (microBlockLink* linkI = fBlockList; linkI != nullptr; prevI = linkI, linkI = linkI->next)
			{
				if (reinterpret_cast<uint32_t(*)(void*, void*)>(mVU.compareStateF)(pState, &linkI->block.pState) == 0)
				{
					if (linkI != fBlockList)
					{
						prevI->next = linkI->next;
						linkI->next = fBlockList;
						fBlockList = linkI;
					}

					return &linkI->block;
				}
			}
		}
		else // Can do Simple Search (Only Matches the Important Pipeline Stuff)
		{
			const uint64_t quick64 = pState->quick64[0];
			for (const microBlockLinkRef& ref : quickLookup)
			{
				if (ref.quick != quick64)
					continue;
				if (doConstProp)
				{
					if ((ref.pBlock->pState.vi15 != pState->vi15))
						continue;
					if ((ref.pBlock->pState.vi15v != pState->vi15v))
						continue;
				}
				return ref.pBlock;
			}
		}
		return nullptr;
	}
};

/* microVU rec structs */
alignas(16) microVU microVU0;
alignas(16) microVU microVU1;

/* Main Functions */
extern void mVUclear(mV, uint32_t, uint32_t);
extern void mVUreset(microVU& mVU, bool resetReserve);
extern void* mVUblockFetch(microVU& mVU, uint32_t startPC, uintptr_t pState);
_mVUt extern void* mVUcompileJIT(uint32_t startPC, uintptr_t ptr);

/* Prototypes */
extern void mVUcleanUpVU0(void);
extern void mVUcleanUpVU1(void);
mVUop(mVUopU);
mVUop(mVUopL);

/* Private Functions */
extern void mVUcacheProg(microVU& mVU, microProgram& prog);
extern void mVUdeleteProg(microVU& mVU, microProgram*& prog);
_mVUt extern void* mVUsearchProg(uint32_t startPC, uintptr_t pState);
extern void* mVUexecuteVU0(uint32_t startPC, uint32_t cycles);
extern void* mVUexecuteVU1(uint32_t startPC, uint32_t cycles);

/* recCall Function Pointer */
typedef void (*mVUrecCall)(uint32_t, uint32_t);
typedef void (*mVUrecCallXG)(void);
