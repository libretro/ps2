/*  IOP branch conditions against console captures.
 *
 *  Scores whether each branch in ps2autotests tests/cpu/iop/branch.expected
 *  is taken, and whether it links, against the implementations in
 *  pcsx2/R3000AInterpreter.cpp.
 *
 *  The capture reports three things per line -- followed or skipped, ra
 *  set or ignored, and whether the delay slot ran -- and two of them are
 *  pure. Whether a branch is taken is a function of its operands, and
 *  whether it links is a property of the opcode. Only the delay slot
 *  needs the surrounding control flow, and on an R3000A there is nothing
 *  to decide: it has no likely forms, so every line in the file says the
 *  delay slot ran. That is asserted here rather than assumed, so a
 *  capture that ever says otherwise fails instead of being ignored.
 *
 *  The link behaviour is the part worth pinning. BGEZAL and BLTZAL call
 *  _SetLink(31) before testing the condition, so they write ra even when
 *  the branch is not taken -- the capture agrees, printing "skipped, set
 *  ra" for exactly those cases.
 *
 *  Usage: tests/iop/hwbranch <path-to-iop-branch.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef int32_t  s32;

static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u }, { "C_S16_MAX",  0x00007FFFu },
	{ "C_S16_MIN",  0xFFFF8000u }, { "C_S32_MAX",  0x7FFFFFFFu },
	{ "C_S32_MIN",  0x80000000u }, { "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u }, { "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
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

enum { B_BEQ, B_BGEZ, B_BGEZAL, B_BGTZ, B_BLEZ, B_BLTZ, B_BLTZAL, B_BNE,
       B_J, B_JAL, B_JALR, B_JR, B_COUNT };
static const char* const kName[B_COUNT] = {
	"beq","bgez","bgezal","bgtz","blez","bltz","bltzal","bne",
	"j","jal","jalr","jr" };
/* Two operands, or one, or none for the jumps. */
static int operands(int op)
{
	if (op == B_BEQ || op == B_BNE) return 2;
	if (op >= B_J) return 0;
	return 1;
}
/* Which forms write ra. BGEZAL and BLTZAL do so unconditionally. */
static int links(int op)
{ return op == B_BGEZAL || op == B_BLTZAL || op == B_JAL || op == B_JALR; }

/* Transcribed from R3000AInterpreter.cpp. */
static int taken(int op, u32 rs, u32 rt)
{
	switch (op)
	{
	case B_BEQ:    return rs == rt;
	case B_BNE:    return rs != rt;
	case B_BGEZ:
	case B_BGEZAL: return (s32)rs >= 0;
	case B_BGTZ:   return (s32)rs >  0;
	case B_BLEZ:   return (s32)rs <= 0;
	case B_BLTZ:
	case B_BLTZAL: return (s32)rs <  0;
	default:       return 1;      /* the jumps are unconditional */
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "branch.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < B_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 rs = 0, rt = 0;
			int want_taken, want_link, want_slot;
			char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			p = buf + 2 + strlen(kName[op]);
			if (operands(op) >= 1 && !operand(&p, &rs)) continue;
			if (operands(op) == 2 && !operand(&p, &rt)) continue;
			p = strchr(buf, ':');
			if (!p) continue;

			want_taken = strstr(p, "followed") != NULL;
			want_link  = strstr(p, "set ra")   != NULL;
			want_slot  = strstr(p, "ran delay slot") != NULL;
			if (!want_taken && !strstr(p, "skipped")) continue;

			cases++;
			/* The R3000A has no likely forms, so the delay slot always
			 * runs; assert it rather than skipping the field. */
			if (taken(op, rs, rt) == want_taken
			 && links(op) == want_link
			 && want_slot)
				pass++;
			else if (failures++ < 8)
				printf("  %-7s %08x, %08x: console %s/%s  ours %s/%s\n",
				       kName[op], rs, rt,
				       want_taken ? "followed" : "skipped",
				       want_link ? "set ra" : "ignored ra",
				       taken(op, rs, rt) ? "followed" : "skipped",
				       links(op) ? "set ra" : "ignored ra");
		}
		fclose(f);
		if (cases == 0)
		{ printf("  a block parsed no cases; its name or line shape does not match the capture\n"); failures++; }
		printf("%-7s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("iop hwbranch: %d cases across %d ops\n", total, B_COUNT);
	return failures != 0;
}
