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

	/* 64-bit forms: the REX.W prefix, and mov64_ri's choice between the
	 * sign-extended 32-bit form and the full ten-byte immediate. */
	CASE("mov rax, rcx",         xe_mov64_rr(0, 1));
	CASE("mov r8, r9",           xe_mov64_rr(8, 9));
	CASE("add rax, rcx",         xe_add64_rr(0, 1));
	CASE("add r10, r11",         xe_add64_rr(10, 11));
	CASE("cmp rax, rcx",         xe_cmp64_rr(0, 1));
	CASE("movsxd rax, ecx",      xe_movsxd_rr(0, 1));
	CASE("movsxd r8, r9d",       xe_movsxd_rr(8, 9));

	/* Shifts by an immediate, including the one-bit short form. */
	CASE("shl eax, 1",           xe_shl32_ri(0, 1));
	CASE("shl eax, 5",           xe_shl32_ri(0, 5));
	CASE("shl r12d, 31",         xe_shl32_ri(12, 31));
	CASE("shr eax, 1",           xe_shr32_ri(0, 1));
	CASE("shr ecx, 7",           xe_shr32_ri(1, 7));
	CASE("sar eax, 1",           xe_sar32_ri(0, 1));
	CASE("sar r13d, 12",         xe_sar32_ri(13, 12));
	CASE("shl rax, 1",           xe_shl64_ri(0, 1));
	CASE("shl rax, 40",          xe_shl64_ri(0, 40));
	CASE("shr rcx, 33",          xe_shr64_ri(1, 33));
	CASE("sar rdx, 63",          xe_sar64_ri(2, 63));

	/* Unary forms and test, which the branch paths lean on. */
	CASE("not eax",              xe_not32_r(0));
	CASE("not r14d",             xe_not32_r(14));
	CASE("neg eax",              xe_neg32_r(0));
	CASE("neg r11d",             xe_neg32_r(11));
	CASE("not rax",              xe_not64_r(0));
	CASE("test eax, eax",        xe_test32_rr(0, 0));
	CASE("test ecx, edx",        xe_test32_rr(1, 2));
	CASE("test r8d, r9d",        xe_test32_rr(8, 9));
	CASE("test rax, rax",        xe_test64_rr(0, 0));

	/* Sign and zero extension, where the operand widths differ. */
	CASE("movzx eax, cl",        xe_movzx32_r8(0, 1));
	CASE("movzx eax, cx",        xe_movzx32_r16(0, 1));
	CASE("movzx r8d, r9b",       xe_movzx32_r8(8, 9));

	/* imul32_r is the one-operand form: edx:eax = eax * r. */
	CASE("imul ecx",             xe_imul32_r(1));
	CASE("imul r11d",            xe_imul32_r(11));

	/* SSE. The recompilers emit far more of this than of the integer ALU,
	 * and the register-extension bit lands in a different place from the
	 * general-purpose forms. */
	CASE("movaps xmm0, xmm1",        xe_movaps_xx(0, 1));
	CASE("movaps xmm8, xmm9",        xe_movaps_xx(8, 9));
	CASE("movdqa xmm0, xmm1",        xe_movdqa_xx(0, 1));
	CASE("movdqa xmm10, xmm11",      xe_movdqa_xx(10, 11));
	CASE("paddd xmm0, xmm1",         xe_paddd_xx(0, 1));
	CASE("paddd xmm12, xmm13",       xe_paddd_xx(12, 13));
	CASE("psubd xmm0, xmm1",         xe_psubd_xx(0, 1));
	CASE("pand xmm2, xmm3",          xe_pand_xx(2, 3));
	CASE("por xmm4, xmm5",           xe_por_xx(4, 5));
	CASE("pxor xmm6, xmm7",          xe_pxor_xx(6, 7));
	CASE("pxor xmm15, xmm15",        xe_pxor_xx(15, 15));
	CASE("punpckldq xmm0, xmm1",     xe_punpckldq_xx(0, 1));
	CASE("punpckhdq xmm0, xmm1",     xe_punpckhdq_xx(0, 1));
	CASE("punpcklqdq xmm8, xmm9",    xe_punpcklqdq_xx(8, 9));
	CASE("punpcklbw xmm0, xmm1",     xe_punpcklbw_xx(0, 1));
	CASE("punpckhwd xmm2, xmm3",     xe_punpckhwd_xx(2, 3));

	/* Memory operands. This is where an x86 encoder is easiest to get
	 * wrong -- ModRM, the SIB byte and the displacement all interact --
	 * and the register-only cases above would not catch any of it.
	 *
	 * The addresses here are absolute and fit in 32 bits, which the
	 * emitter encodes with the no-base SIB form (modrm 04, sib 25). The
	 * ds: prefix in the oracle text is how Intel syntax spells the same
	 * thing to as; without it, as assembles a RIP-relative operand and
	 * every case disagrees for a reason that has nothing to do with the
	 * emitter. */
	CASE("add eax, DWORD PTR ds:0x12345678", xe_add32_rm(0, 0x12345678));
	CASE("add ecx, DWORD PTR ds:0x100",      xe_add32_rm(1, 0x100));
	CASE("add r8d, DWORD PTR ds:0x12345678", xe_add32_rm(8, 0x12345678));
	CASE("sub eax, DWORD PTR ds:0x12345678", xe_sub32_rm(0, 0x12345678));
	CASE("cmp eax, DWORD PTR ds:0x12345678", xe_cmp32_rm(0, 0x12345678));
	CASE("mov eax, DWORD PTR ds:0x12345678", xe_mov32_rm(0, 0x12345678));
	CASE("mov r15d, DWORD PTR ds:0x40",      xe_mov32_rm(15, 0x40));
	CASE("mov DWORD PTR ds:0x12345678, ecx", xe_mov32_mr(0x12345678, 1));
	CASE("mov DWORD PTR ds:0x40, r14d",      xe_mov32_mr(0x40, 14));
	CASE("sub DWORD PTR ds:0x12345678, edx", xe_sub32_mr(0x12345678, 2));
	CASE("mov rax, QWORD PTR ds:0x12345678", xe_mov64_rm(0, 0x12345678));
	CASE("mov QWORD PTR ds:0x12345678, rcx", xe_mov64_mr(0x12345678, 1));
	CASE("add rax, QWORD PTR ds:0x12345678", xe_add64_rm(0, 0x12345678));

	/* Immediates to memory, where the operand size has to be spelled out
	 * because neither side can infer it. */
	CASE("mov DWORD PTR ds:0x12345678, 0x1",        xe_mov32_mi(0x12345678, 1));
	CASE("mov DWORD PTR ds:0x100, 0xdeadbeef",      xe_mov32_mi(0x100, 0xdeadbeef));
	CASE("cmp DWORD PTR ds:0x12345678, 0x7f",       xe_cmp32_mi(0x12345678, 0x7f));
	CASE("cmp DWORD PTR ds:0x12345678, 0x12345678", xe_cmp32_mi(0x12345678, 0x12345678));
	CASE("sub DWORD PTR ds:0x100, 0x10",            xe_sub32_mi(0x100, 0x10));

	printf("hwas: %d/%d C emitter macros match GNU as\n",
	       cases - failures, cases);
	return failures != 0;
}
