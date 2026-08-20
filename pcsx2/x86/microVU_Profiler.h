// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

enum microOpcode
{
	// Upper Instructions
	opABS, opCLIP, opOPMULA, opOPMSUB, opNOP,
	opADD,   opADDi,   opADDq,   opADDx,   opADDy,   opADDz,   opADDw,
	opADDA,  opADDAi,  opADDAq,  opADDAx,  opADDAy,  opADDAz,  opADDAw,
	opSUB,   opSUBi,   opSUBq,   opSUBx,   opSUBy,   opSUBz,   opSUBw,
	opSUBA,  opSUBAi,  opSUBAq,  opSUBAx,  opSUBAy,  opSUBAz,  opSUBAw,
	opMUL,   opMULi,   opMULq,   opMULx,   opMULy,   opMULz,   opMULw,
	opMULA,  opMULAi,  opMULAq,  opMULAx,  opMULAy,  opMULAz,  opMULAw,
	opMADD,  opMADDi,  opMADDq,  opMADDx,  opMADDy,  opMADDz,  opMADDw,
	opMADDA, opMADDAi, opMADDAq, opMADDAx, opMADDAy, opMADDAz, opMADDAw,
	opMSUB,  opMSUBi,  opMSUBq,  opMSUBx,  opMSUBy,  opMSUBz,  opMSUBw,
	opMSUBA, opMSUBAi, opMSUBAq, opMSUBAx, opMSUBAy, opMSUBAz, opMSUBAw,
	opMAX,   opMAXi,             opMAXx,   opMAXy,    opMAXz,  opMAXw,
	opMINI,  opMINIi,            opMINIx,  opMINIy,   opMINIz, opMINIw,
	opFTOI0, opFTOI4, opFTOI12, opFTOI15,
	opITOF0, opITOF4, opITOF12, opITOF15,
	// Lower Instructions
	opDIV, opSQRT, opRSQRT,
	opIADD, opIADDI, opIADDIU,
	opIAND, opIOR,
	opISUB, opISUBIU,
	opMOVE, opMFIR, opMTIR, opMR32, opMFP,
	opLQ, opLQD, opLQI,
	opSQ, opSQD, opSQI,
	opILW, opISW, opILWR, opISWR,
	opRINIT, opRGET, opRNEXT, opRXOR,
	opWAITQ, opWAITP,
	opFSAND, opFSEQ, opFSOR, opFSSET,
	opFMAND, opFMEQ, opFMOR,
	opFCAND, opFCEQ, opFCOR, opFCSET, opFCGET,
	opIBEQ, opIBGEZ, opIBGTZ, opIBLTZ, opIBLEZ, opIBNE,
	opB, opBAL, opJR, opJALR,
	opESADD, opERSADD, opELENG, opERLENG,
	opEATANxy, opEATANxz, opESUM, opERCPR,
	opESQRT, opERSQRT, opESIN, opEATAN,
	opEEXP, opXITOP, opXTOP, opXGKICK,
	opLastOpcode
};

static const char microOpcodeName[][16] = {
	// Upper Instructions
	"ABS", "CLIP", "OPMULA", "OPMSUB", "NOP",
	"ADD",   "ADDi",   "ADDq",   "ADDx",   "ADDy",   "ADDz",   "ADDw",
	"ADDA",  "ADDAi",  "ADDAq",  "ADDAx",  "ADDAy",  "ADDAz",  "ADDAw",
	"SUB",   "SUBi",   "SUBq",   "SUBx",   "SUBy",   "SUBz",   "SUBw",
	"SUBA",  "SUBAi",  "SUBAq",  "SUBAx",  "SUBAy",  "SUBAz",  "SUBAw",
	"MUL",   "MULi",   "MULq",   "MULx",   "MULy",   "MULz",   "MULw",
	"MULA",  "MULAi",  "MULAq",  "MULAx",  "MULAy",  "MULAz",  "MULAw",
	"MADD",  "MADDi",  "MADDq",  "MADDx",  "MADDy",  "MADDz",  "MADDw",
	"MADDA", "MADDAi", "MADDAq", "MADDAx", "MADDAy", "MADDAz", "MADDAw",
	"MSUB",  "MSUBi",  "MSUBq",  "MSUBx",  "MSUBy",  "MSUBz",  "MSUBw",
	"MSUBA", "MSUBAi", "MSUBAq", "MSUBAx", "MSUBAy", "MSUBAz", "MSUBAw",
	"MAX",   "MAXi",             "MAXx",   "MAXy",   "MAXz",   "MAXw",
	"MINI",  "MINIi",            "MINIx",  "MINIy",  "MINIz",  "MINIw",
	"FTOI0", "FTOI4", "FTOI12", "FTOI15",
	"ITOF0", "ITOF4", "ITOF12", "ITOF15",
	// Lower Instructions
	"DIV", "SQRT", "RSQRT",
	"IADD", "IADDI", "IADDIU",
	"IAND", "IOR",
	"ISUB", "ISUBIU",
	"MOVE", "MFIR", "MTIR", "MR32", "MFP",
	"LQ", "LQD", "LQI",
	"SQ", "SQD", "SQI",
	"ILW", "ISW", "ILWR", "ISWR",
	"RINIT", "RGET", "RNEXT", "RXOR",
	"WAITQ", "WAITP",
	"FSAND", "FSEQ", "FSOR", "FSSET",
	"FMAND", "FMEQ", "FMOR",
	"FCAND", "FCEQ", "FCOR", "FCSET", "FCGET",
	"IBEQ", "IBGEZ", "IBGTZ", "IBLTZ", "IBLEZ", "IBNE",
	"B", "BAL", "JR", "JALR",
	"ESADD", "ERSADD", "ELENG", "ERLENG",
	"EATANxy", "EATANxz", "ESUM", "ERCPR",
	"ESQRT", "ERSQRT", "ESIN", "EATAN",
	"EEXP", "XITOP", "XTOP", "XGKICK"
};

#ifdef mVUprofileProg
#include <utility>
#include <string>
#include <algorithm>

struct microProfiler
{
	static const u32 progLimit = 10000;
	u64 opStats[opLastOpcode];
	u32 progCount;
	int index;
	void Reset(int _index)
	{
		memset(this, 0, sizeof(*this));
		index = _index;
	}
	void EmitOp(microOpcode op)
	{
		xe_add32_mi(&(((u32*)opStats)[op * 2 + 0]), 1);
		xe_adc32_mi(&(((u32*)opStats)[op * 2 + 1]), 0);
	}
	void Print()
	{
		progCount++;
		if ((progCount % progLimit) == 0)
		{
			/* C89 shape: fixed table + insertion sort descending by count. */
			u64 total = 0;
			u64 counts[opLastOpcode];
			int order[opLastOpcode];
			int i, j;
			char name[16];
			for (i = 0; i < opLastOpcode; i++)
			{
				counts[i] = opStats[i];
				order[i] = i;
				total += counts[i];
			}
			for (i = 1; i < opLastOpcode; i++)
			{
				const int oi = order[i];
				const u64 ci = counts[oi];
				for (j = i; j > 0 && counts[order[j - 1]] < ci; j--)
					order[j] = order[j - 1];
				order[j] = oi;
			}
			DevCon.WriteLn("microVU%d Profiler:", index);
			for (i = 0; i < opLastOpcode; i++)
			{
				const int op = order[i];
				const u64 count = counts[op];
				const double stat = (double)count / (double)total * 100.0;
				int n = 0;
				while (n < 8 && microOpcodeName[op][n]) { name[n] = microOpcodeName[op][n]; n++; }
				while (n < 8) name[n++] = ' ';
				name[n] = 0;
				DevCon.WriteLn("%s - [%3.4f%%][count=%u]", name, stat, (u32)count);
			}
			DevCon.WriteLn("Total = 0x%x%x\n\n", (u32)(u64)(total >> 32), (u32)total);
		}
	}
};
#else
struct microProfiler
{
	__fi void Reset(int _index) {}
	__fi void EmitOp(microOpcode op) {}
	__fi void Print() {}
};
#endif
