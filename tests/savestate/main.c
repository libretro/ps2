/* The savestate block writer and reader, round-tripped.
 *
 * SaveState_FreezeMem grows its own allocation while saving and bounds the
 * cursor against the live size while loading, so the two directions have to
 * agree about where every block lands. This writes a spread of blocks,
 * reads them back through a second pass over the same bytes, and checks
 * both the contents and the cursor at each step.
 *
 * The growth path is the part worth pushing: a state is built out of many
 * small blocks, so the reserve starts from nothing and doubles, and a block
 * that straddles a growth boundary must still come back whole.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SaveState.h"

static long checks;
static long failures;

static void fail(const char *what)
{
	printf("  FAIL: %s\n", what);
	failures++;
}

static void expect(int cond, const char *what)
{
	checks++;
	if (!cond)
		fail(what);
}

static unsigned rs = 0x12345678u;
static unsigned rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

#define NBLOCKS 400

int main(void)
{
	static u8  src[NBLOCKS][600];
	static u8  dst[NBLOCKS][600];
	static int len[NBLOCKS];
	static int pos[NBLOCKS];
	SaveStateBase s;
	int i, j;

	for (i = 0; i < NBLOCKS; i++)
	{
		/* Sizes either side of the doubling steps, plus the degenerate
		 * zero-length block, which has to move the cursor not at all. */
		len[i] = (i % 17 == 0) ? 0 : (int)(rnd() % 600u) + 1;
		for (j = 0; j < len[i]; j++)
			src[i][j] = (u8)rnd();
	}

	/* ---- saving: no allocation to begin with, so every growth step runs */
	SaveState_Init(&s, NULL, 0, 0, true);
	expect(SaveState_IsSaving(&s), "Init picked the saving direction");
	expect(SaveState_IsOkay(&s), "a fresh writer is not in error");

	for (i = 0; i < NBLOCKS; i++)
	{
		pos[i] = s.idx;
		SaveState_FreezeMem(&s, src[i], len[i]);
		expect(s.idx == pos[i] + len[i], "the cursor moved by the block size");
	}
	expect(SaveState_IsOkay(&s), "writing the whole state stayed clean");
	expect(s.memory_size == (size_t)s.idx, "the live size tracks the cursor");
	expect(s.memory_cap >= s.memory_size, "the allocation covers the live size");

	/* ---- loading: the same bytes back, through a second pass */
	{
		SaveStateBase l;
		SaveState_Init(&l, s.memory, s.memory_size, s.memory_cap, false);
		expect(SaveState_IsLoading(&l), "Init picked the loading direction");

		for (i = 0; i < NBLOCKS; i++)
		{
			memset(dst[i], 0xcd, sizeof(dst[i]));
			expect(l.idx == pos[i], "the reader is where the writer was");
			SaveState_FreezeMem(&l, dst[i], len[i]);
			if (len[i] && memcmp(dst[i], src[i], (size_t)len[i]) != 0)
				fail("a block did not come back as it went in");
			checks++;
		}
		expect(SaveState_IsOkay(&l), "reading the whole state stayed clean");
		expect(l.idx == s.idx, "both passes ended at the same offset");
	}

	/* ---- a reader given too little has to say so rather than read on */
	{
		SaveStateBase l;
		u8 out[64];

		SaveState_Init(&l, s.memory, 32, s.memory_cap, false);
		SaveState_PrepBlock(&l, 64);
		expect(!SaveState_IsOkay(&l), "a block past the end raises the error");

		/* and once it has, loads come back zeroed rather than as
		 * whatever happened to be there */
		memset(out, 0xcd, sizeof(out));
		SaveState_FreezeMem(&l, out, (int)sizeof(out));
		for (j = 0; j < (int)sizeof(out); j++)
			if (out[j] != 0)
			{
				fail("a load after an error did not zero its destination");
				break;
			}
		checks++;
	}

	/* ---- the tag round-trips, and a wrong one is caught */
	{
		SaveStateBase w, l;

		SaveState_Init(&w, NULL, 0, 0, true);
		expect(SaveState_FreezeTag(&w, "cpuRegs"), "a tag writes cleanly");

		SaveState_Init(&l, w.memory, w.memory_size, w.memory_cap, false);
		expect(SaveState_FreezeTag(&l, "cpuRegs"), "the same tag reads back");

		SaveState_Init(&l, w.memory, w.memory_size, w.memory_cap, false);
		expect(!SaveState_FreezeTag(&l, "iopRegs"), "a different tag is rejected");
		expect(!SaveState_IsOkay(&l), "and leaves the reader in error");

		free(w.memory);
	}

	free(s.memory);

	printf("%s: savestate blocks, %ld checks, %ld failures\n",
	       failures ? "FAIL" : "PASS", checks, failures);
	return failures != 0;
}
