/*  The C emitter macros against GNU as.
 *
 *  tests/emitter/oracle.cpp compared the C++ emitter against the C macros
 *  in common/emitter/c89ops.h. The C++ side has since been removed --
 *  xRegisterBase, xIndirectVoid and the JMP8/JZ8 family survive only in
 *  the comments that say which macro mirrors which -- so that file cannot
 *  build and there is nothing left for it to compare against.
 *
 *  The macros themselves are what every recompiler now emits through, and
 *  nothing tested them. This does, against an oracle that cannot rot the
 *  same way: GNU as. Each case names an instruction in Intel syntax, this
 *  assembles it, and the bytes are compared with what the macro emits.
 *
 *  Being an external assembler rather than a second copy of the same
 *  logic, it disagrees for real reasons. A macro that mirrors the old C++
 *  emitter's quirks rather than the architecture would show up here and
 *  not in a comparison against that emitter.
 *
 *  Needs `as` and `objdump` on PATH; skips cleanly if either is missing.
 *
 *  Usage: tests/emitter/hwas
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "common/emitter/x86types.h"
#include "common/emitter/c89ops.h"

/* Somewhere to emit into. */
static uint8_t s_buf[256];

/* Assemble one Intel-syntax instruction and return its encoding. */
static int assemble(const char* insn, uint8_t* out, int max)
{
	char cmd[1024];
	FILE* f;
	int n = 0;
	unsigned v;

	f = fopen("/tmp/xe_oracle.s", "w");
	if (!f) return -1;
	fprintf(f, ".intel_syntax noprefix\n%s\n", insn);
	fclose(f);

	if (system("as -o /tmp/xe_oracle.o /tmp/xe_oracle.s 2>/dev/null") != 0)
		return -1;

	snprintf(cmd, sizeof(cmd),
	         "objdump -d --insn-width=16 /tmp/xe_oracle.o"
	         " | sed -n 's/^ *[0-9a-f]*:\\t\\([0-9a-f ]*\\)\\t.*/\\1/p'");
	f = popen(cmd, "r");
	if (!f) return -1;
	while (n < max && fscanf(f, "%2x", &v) == 1)
		out[n++] = (uint8_t)v;
	pclose(f);
	return n;
}

static int failures, cases, shown;

static void check(const char* insn, void (*emit)(void))
{
	uint8_t want[64];
	int nwant, ngot, i;

	nwant = assemble(insn, want, (int)sizeof(want));
	if (nwant <= 0)
	{
		printf("  %-32s could not be assembled; skipped\n", insn);
		return;
	}

	x86Ptr = s_buf;
	emit();
	ngot = (int)(x86Ptr - s_buf);

	cases++;
	if (ngot == nwant && !memcmp(s_buf, want, (size_t)nwant)) return;

	failures++;
	if (shown++ < 12)
	{
		printf("  %-32s as:", insn);
		for (i = 0; i < nwant; i++) printf(" %02x", want[i]);
		printf("   macro:");
		for (i = 0; i < ngot; i++) printf(" %02x", s_buf[i]);
		printf("\n");
	}
}

/* Each case is the instruction text and a thunk that emits it. Kept
 * side by side so a mismatch names the instruction rather than a line. */
#define CASE(text, body) \
	do { struct L { static void f(void) { body; } }; check(text, &L::f); } while (0)

int main(void)
{
	if (system("as --version >/dev/null 2>&1") != 0
	 || system("objdump --version >/dev/null 2>&1") != 0)
	{
		printf("hwas: as or objdump not found; skipped\n");
		return 0;
	}

	/* Register-to-register ALU, low and extended registers, since the REX
	 * prefix is where an encoder most often goes wrong. */
	CASE("add eax, ecx",     xe_add32_rr(0, 1));
	CASE("add ecx, eax",     xe_add32_rr(1, 0));
	CASE("add r8d, r9d",     xe_add32_rr(8, 9));
	CASE("add eax, r15d",    xe_add32_rr(0, 15));
	CASE("add r15d, eax",    xe_add32_rr(15, 0));
	CASE("sub eax, ecx",     xe_sub32_rr(0, 1));
	CASE("sub r10d, r11d",   xe_sub32_rr(10, 11));
	CASE("xor eax, eax",     xe_xor32_rr(0, 0));
	CASE("xor r12d, r13d",   xe_xor32_rr(12, 13));
	CASE("and eax, ecx",     xe_and32_rr(0, 1));
	CASE("or  eax, ecx",     xe_or32_rr(0, 1));
	CASE("cmp eax, ecx",     xe_cmp32_rr(0, 1));

	/* Immediates: the short form for a sign-extendable byte and the long
	 * form otherwise, and the eax special case. */
	CASE("add eax, 1",           xe_add32_ri(0, 1));
	CASE("add eax, 0x7f",        xe_add32_ri(0, 0x7f));
	CASE("add eax, 0x80",        xe_add32_ri(0, 0x80));
	CASE("add eax, 0x12345678",  xe_add32_ri(0, 0x12345678));
	CASE("add ecx, 1",           xe_add32_ri(1, 1));
	CASE("add ecx, 0x12345678",  xe_add32_ri(1, 0x12345678));
	CASE("add r8d, 1",           xe_add32_ri(8, 1));
	CASE("sub eax, 0x7fffffff",  xe_sub32_ri(0, 0x7fffffff));
	CASE("cmp eax, 0",           xe_cmp32_ri(0, 0));

	/* Moves. */
	CASE("mov eax, ecx",         xe_mov32_rr(0, 1));
	CASE("mov r8d, r9d",         xe_mov32_rr(8, 9));
	/* E_MOV_RI_SZ turns a zero immediate into XOR reg,reg deliberately --
	 * it saves bytes and the comment there says the C++ emitter did the
	 * same. So the oracle instruction is the XOR: comparing against a
	 * literal mov would be testing that the optimisation is absent. */
	CASE("xor eax, eax",         xe_mov32_ri(0, 0));
	CASE("xor r13d, r13d",       xe_mov32_ri(13, 0));
	CASE("mov eax, 0x12345678",  xe_mov32_ri(0, 0x12345678));
	CASE("mov r15d, 0x1",        xe_mov32_ri(15, 1));

	printf("hwas: %d/%d C emitter macros match GNU as\n",
	       cases - failures, cases);
	return failures != 0;
}
