/*  IOP loads and stores against console captures.
 *
 *  Scores LB, LBU, LH, LHU, LW, the LWL/LWR pair, SB, SH, SW and the
 *  SWL/SWR pair against ps2autotests tests/cpu/iop/lsu.expected, using
 *  the semantics in pcsx2/R3000AOpcodeTables.cpp.
 *
 *  The memory is the same three-row pattern the EE lsu test uses, and it
 *  can be read straight out of the capture rather than guessed at: the LW
 *  block prints 23456789 at +0, abcdef01 at +4 and c0de1337 at -4, which
 *  places the base at byte 16 and fixes every word around it.
 *
 *  The stores do not share that base. They print 9abcde21 for "sb +0",
 *  which is the word at byte 4 with 0x21 written over its low byte, so
 *  the store harness bases at byte 4 while the load harness bases at byte
 *  16 -- a flat array index against a row index of a [3][4]. Using one
 *  base for both makes every store case fail while the loads pass, which
 *  reads as a broken store path rather than a misread harness.
 *
 *  Each store line prints the word the store landed in and the one after
 *  it, so a write that spills into its neighbour is visible. Stores run
 *  two rt seeds, the immediate one and then C_GARBAGE1, which shows in
 *  the capture as the written byte changing from 0x21 to 0x37 halfway
 *  down each block.
 *
 *  The LWL/LWR block is different again: rt is never reseeded, so the
 *  whole block is one chain. That is visible in the numbers -- "lwr +0"
 *  yields 23456789, and the "lwl +1" after it yields 67896789, which is
 *  that result shifted in rather than anything a fresh seed would give.
 *  Its starting value only shows through the low three bytes (0xde1337);
 *  the top byte is never read before something overwrites it, so it is
 *  unobservable here and set to zero.
 *
 *  Usage: tests/iop/hwlsu <path-to-iop-lsu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

static const u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
#define LOAD_BASE   16   /* C_PATTERN[1], a row of the [3][4] */
#define STORE_BASE   4   /* &buffer[1], a flat word index */

static u8 mem[48];
static void reset_mem(void) { memcpy(mem, kPattern, sizeof(mem)); }
static int inb(int i) { return i >= 0 && i < 48; }

static u32 rd8(int off)  { const int i = off; return inb(i) ? mem[i] : 0; }
static u32 rd16(int off) { return rd8(off) | (rd8(off+1) << 8); }
static u32 rd32(int off) { return rd16(off) | (rd16(off+2) << 16); }
static void wr(int off, u32 v, int bytes)
{ int k; for (k = 0; k < bytes; k++) if (inb(off+k)) mem[off+k] = (u8)(v >> (8*k)); }

/* Store seeds: the immediate one, then C_GARBAGE1's low word. */
#define RT_STORE_A 0xABCD4321u
#define RT_STORE_B 0x00001337u
/* LWL/LWR chain start; only the low three bytes are observable. */
#define RT_LWLR    0x00de1337u

enum { L_LB, L_LBU, L_LH, L_LHU, L_LW, L_LWLR,
       S_SB, S_SH, S_SW, S_SWLR, K_COUNT };
static const char* const kName[K_COUNT] = {
	"lb", "lbu", "lh", "lhu", "lw", "lwl / lwr",
	"sb", "sh", "sw", "swl / swr" };
static int is_store(int op) { return op >= S_SB; }

/* Transcribed from R3000AOpcodeTables.cpp. */
/* Case count for a block, so the store seed can switch at its midpoint:
 * the byte, halfword and word blocks run three offsets twice, the
 * unaligned pairs five. Assuming three seeds the wrong half of the larger
 * blocks and fails cases that are actually correct. */
static int count_block(const char* path, const char* name)
{
	FILE* f = fopen(path, "r");
	char buf[512];
	int in = 0, n = 0;
	if (!f) return 0;
	while (fgets(buf, sizeof(buf), f))
	{
		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			if (in) break;
			if (c && c[1] == '\n') { *c = '\0'; in = !strcmp(buf, name); *c = ':'; }
			continue;
		}
		if (!in || strstr(buf, "-> $0")) continue;
		n++;
	}
	fclose(f);
	return n;
}

static u32 do_lwl(u32 rt, int off)
{
	const u32 shift = (u32)(off & 3) << 3;
	const u32 mem32 = rd32(off & ~3);
	return (rt & (0x00ffffffu >> shift)) | (mem32 << (24 - shift));
}
static u32 do_lwr(u32 rt, int off)
{
	const u32 shift = (u32)(off & 3) << 3;
	const u32 mem32 = rd32(off & ~3);
	return (rt & (0xffffff00u << (24 - shift))) | (mem32 >> shift);
}
static void do_swl(u32 rt, int off)
{
	const u32 shift = (u32)(off & 3) << 3;
	const int a = off & ~3;
	wr(a, (rd32(a) & (0xffffff00u << shift)) | (rt >> (24 - shift)), 4);
}
static void do_swr(u32 rt, int off)
{
	const u32 shift = (u32)(off & 3) << 3;
	const int a = off & ~3;
	wr(a, (rd32(a) & (0x00ffffffu >> (24 - shift))) | (rt << shift), 4);
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "lsu.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int op = -1, failures = 0, total = 0, i;
	int pass[K_COUNT], cases[K_COUNT];
	u32 rt_chain = RT_LWLR;   /* carried across the LWL/LWR block */
	int total_in_block[K_COUNT];

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < K_COUNT; i++)
	{
		pass[i] = 0; cases[i] = 0;
		total_in_block[i] = count_block(path, kName[i]);
	}

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w0, w1;
		int off1 = 0, off2 = 0, n = 0;
		char op1[8], op2[8];
		u32 got0, got1 = 0, rt;
		char* p;

		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			op = -1;
			if (c && c[1] == '\n')
			{
				*c = '\0';
				for (i = 0; i < K_COUNT; i++)
					if (!strcmp(buf, kName[i])) { op = i; break; }
				*c = ':';
			}
			continue;
		}
		if (op < 0 || strstr(buf, "-> $0")) continue;

		/* "  <op> <+off>[/<op> <+off>]: <word>[ <word>]" */
		p = buf + 2;
		{ int k = 0; while (*p && *p != ' ' && k < 7) op1[k++] = *p++; op1[k] = '\0'; }
		while (*p == ' ') p++;
		if (*p == '+') p++;
		off1 = (int)strtol(p, &p, 10);
		if (*p == '/')
		{
			int k = 0; p++;
			while (*p && *p != ' ' && k < 7) op2[k++] = *p++;
			op2[k] = '\0';
			while (*p == ' ') p++;
			if (*p == '+') p++;
			off2 = (int)strtol(p, &p, 10);
			n = 2;
		}
		p = strchr(buf, ':');
		if (!p) continue;

		reset_mem();
		/* Stores run the first half of each block from one seed and the
		 * second half from the other. */
		rt = (cases[op] < total_in_block[op] / 2) ? RT_STORE_A : RT_STORE_B;

		if (!is_store(op))
		{
			const int b = LOAD_BASE + off1;
			if (sscanf(p + 1, " %x", &w0) != 1) continue;
			switch (op)
			{
			case L_LB:   got0 = (u32)(s32)(s8)rd8(b);      break;
			case L_LBU:  got0 = rd8(b);                    break;
			case L_LH:   got0 = (u32)(s32)(s16)rd16(b);    break;
			case L_LHU:  got0 = rd16(b);                   break;
			case L_LW:   got0 = rd32(b);                   break;
			default:
				got0 = !strcmp(op1, "lwl") ? do_lwl(rt_chain, b) : do_lwr(rt_chain, b);
				if (n == 2)
				{
					const int b2 = LOAD_BASE + off2;
					got0 = !strcmp(op2, "lwl") ? do_lwl(got0, b2) : do_lwr(got0, b2);
				}
				rt_chain = got0;
				break;
			}
			cases[op]++;
			if (got0 == w0) pass[op]++;
			else if (failures++ < 6)
				printf("  %-9s %+d: console %08x  ours %08x\n",
				       kName[op], off1, w0, got0);
			continue;
		}

		{
			const int b = STORE_BASE + off1;
			if (sscanf(p + 1, " %x %x", &w0, &w1) != 2) continue;
			switch (op)
			{
			case S_SB: wr(b, rt, 1); break;
			case S_SH: wr(b, rt, 2); break;
			case S_SW: wr(b, rt, 4); break;
			default:
				if (!strcmp(op1, "swl")) do_swl(rt, b); else do_swr(rt, b);
				if (n == 2)
				{
					const int b2 = STORE_BASE + off2;
					if (!strcmp(op2, "swl")) do_swl(rt, b2); else do_swr(rt, b2);
				}
				break;
			}
			got0 = rd32(b & ~3);
			got1 = rd32((b & ~3) + 4);
			cases[op]++;
			if (got0 == w0 && got1 == w1) pass[op]++;
			else if (failures++ < 6)
				printf("  %-9s %+d: console %08x %08x  ours %08x %08x\n",
				       kName[op], off1, w0, w1, got0, got1);
		}
	}
	fclose(f);

	for (i = 0; i < K_COUNT; i++)
	{
		printf("%-9s %2d/%-3d console cases\n", kName[i], pass[i], cases[i]);
		total += cases[i];
		if (pass[i] != cases[i]) failures++;
	}
	printf("iop hwlsu: %d cases across %d ops\n", total, K_COUNT);
	return failures != 0;
}
