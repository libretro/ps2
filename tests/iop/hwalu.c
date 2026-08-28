/*  IOP ALU against console captures.
 *
 *  Scores the twenty-one ops in ps2autotests tests/cpu/iop/alu.expected
 *  against pcsx2/R3000AOpcodeTables.cpp.
 *
 *  Simpler than the EE equivalent in tests/ee/hwalu.c: the R3000A's
 *  registers are 32 bits, so each line prints a single word and there is
 *  no upper half to carry between cases. What does carry over from that
 *  file is the shape of the traps -- operands printed as decimals first
 *  and constant names after, and the variable shifts taking their
 *  operands in the opposite order to everything else.
 *
 *  Usage: tests/iop/hwalu <path-to-iop-alu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t  s16;
typedef int32_t  s32;

static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u }, { "C_S16_MAX",  0x00007FFFu },
	{ "C_S16_MIN",  0xFFFF8000u }, { "C_S32_MAX",  0x7FFFFFFFu },
	{ "C_S32_MIN",  0x80000000u }, { "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u }, { "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
	/* As in hwmuldiv.c: no sources ship for the IOP tests, so these come
	 * from the EE harness's shared.h under the same names, narrowed to the
	 * low word. */
	{ "C_S64_MAX",  0xFFFFFFFFu }, { "C_S64_MIN",  0x00000000u },
	{ "C_TWOTWOTWOTWO", 0x00000002u }, { "C_ONEONEONEONE", 0x00000001u },
};

static int operand(char** pp, u32* out)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',') p++;
	if (*p == 'C' && p[1] == '_')
	{
		size_t i, best = (size_t)-1, bestlen = 0;
		for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		{
			const size_t n = strlen(kConst[i].name);
			if (!strncmp(p, kConst[i].name, n) && n > bestlen)
			{ best = i; bestlen = n; }
		}
		if (best == (size_t)-1) return 0;
		*out = kConst[best].v;
		p += bestlen;
	}
	else
	{
		long v; char* e;
		v = strtol(p, &e, 10);
		if (e == p) return 0;
		*out = (u32)v;
		p = e;
	}
	*pp = p;
	return 1;
}

enum { O_ADDU, O_ADDIU, O_AND, O_ANDI, O_LUI, O_NOR, O_OR, O_ORI,
       O_SLL, O_SLLV, O_SLT, O_SLTI, O_SLTIU, O_SLTU, O_SRA, O_SRAV,
       O_SRL, O_SRLV, O_SUBU, O_XOR, O_XORI, O_COUNT };
static const char* const kName[O_COUNT] = {
	"addu","addiu","and","andi","lui","nor","or","ori",
	"sll","sllv","slt","slti","sltiu","sltu","sra","srav",
	"srl","srlv","subu","xor","xori" };

/* LUI prints one operand; the rest print two. */
static int one_operand(int op) { return op == O_LUI; }
/* Transcribed from R3000AOpcodeTables.cpp. */
static u32 apply(int op, u32 a, u32 b)
{
	switch (op)
	{
	case O_ADDU:  return a + b;
	case O_SUBU:  return a - b;
	case O_AND:   return a & b;
	case O_OR:    return a | b;
	case O_XOR:   return a ^ b;
	case O_NOR:   return ~(a | b);
	case O_SLT:   return ((s32)a < (s32)b) ? 1u : 0u;
	case O_SLTU:  return (a < b) ? 1u : 0u;
	/* Immediate forms: sign-extended for the arithmetic and compares,
	 * zero-extended for the logicals, exactly as the interpreter has it. */
	case O_ADDIU: return a + (u32)(s32)(s16)(u16)b;
	case O_ANDI:  return a & (u32)(u16)b;
	case O_ORI:   return a | (u32)(u16)b;
	case O_XORI:  return a ^ (u32)(u16)b;
	case O_SLTI:  return ((s32)a < (s32)(s16)(u16)b) ? 1u : 0u;
	/* SLTIU sign-extends its immediate and then compares unsigned, which
	 * is what the interpreter does. This capture does not actually pin
	 * that down -- zero-extending instead still scores 21 of 21, because
	 * the cases with the immediate's top bit set land the same way either
	 * way. ADDIU is checked by the same file and does discriminate,
	 * failing 6 of 19 when zero-extended, so the gap is specific to this
	 * op rather than to the immediates generally. */
	case O_SLTIU: return (a < (u32)(s32)(s16)(u16)b) ? 1u : 0u;
	case O_LUI:   return b << 16;
	/* Shifts: the value is `a`, the amount `b`, five bits of it. */
	case O_SLL: case O_SLLV: return a << (b & 0x1f);
	case O_SRL: case O_SRLV: return a >> (b & 0x1f);
	default:                 return (u32)((s32)a >> (b & 0x1f));
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "alu.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < O_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 a = 0, b = 0, got;
			unsigned want;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			p = buf + 2 + strlen(kName[op]);
			if (one_operand(op))
			{ if (!operand(&p, &b)) continue; }
			else
			{
				if (!operand(&p, &a)) continue;
				if (!operand(&p, &b)) continue;
			}
			p = strchr(buf, ':');
			if (!p || sscanf(p + 1, " %x", &want) != 1) continue;

			got = apply(op, a, b);
			cases++;
			if (got == (u32)want) pass++;
			else if (failures++ < 6)
				printf("  %-6s %08x, %08x: console %08x  ours %08x\n",
				       kName[op], a, b, want, got);
		}
		fclose(f);
		printf("%-6s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("iop hwalu: %d cases across %d ops\n", total, O_COUNT);
	return failures != 0;
}
