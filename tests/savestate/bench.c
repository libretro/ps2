/* The savestate block writer, old against new, on the same state.
 *
 * SaveStateBase was a C++ class holding a std::vector<u8>& and growing it
 * with resize(). It is now a C struct holding its own block and growing it
 * with realloc(). Both do the same work -- copy a few thousand blocks in,
 * then copy them back out -- so a timing comparison between them is a fair
 * one, which it would not be if the work differed.
 *
 * Two things separate them, and both show up in the codegen:
 *
 *   - the class held a *reference* to the vector, so reaching a byte of the
 *     state was two dependent loads (the reference, then the vector's data
 *     pointer). The struct holds the pointer inline, so it is one.
 *
 *   - vector::resize value-initialises every byte it adds. A save pass
 *     therefore zeroes the whole state and then memcpy's over it, which is
 *     an entire second pass over the buffer that realloc does not make.
 *
 * Interleaved best-of-N: the two are run alternately within each trial and
 * the best time for each is kept, so drift on a shared machine cannot
 * decide the result.
 *
 * Built by build.sh --bench. The old class is reproduced here verbatim from
 * what it was before the conversion; it has no other home now.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_old.h"

#include "SaveState.h"

enum { NBLOCKS = 4000 };

static int sizes[NBLOCKS];
static u8  scratch[1 << 16];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static double run_new(int reps)
{
	double t0 = now();
	int r, i;

	for (r = 0; r < reps; r++)
	{
		SaveStateBase s, l;

		SaveState_Init(&s, NULL, 0, 0, true);
		for (i = 0; i < NBLOCKS; i++)
			SaveState_FreezeMem(&s, scratch, sizes[i]);

		SaveState_Init(&l, s.memory, s.memory_size, s.memory_cap, false);
		for (i = 0; i < NBLOCKS; i++)
			SaveState_FreezeMem(&l, scratch, sizes[i]);

		free(s.memory);
	}
	return now() - t0;
}

int main(int argc, char **argv)
{
	int trials = argc > 1 ? atoi(argv[1]) : 15;
	int reps   = argc > 2 ? atoi(argv[2]) : 20;
	unsigned rs = 0x12345678u;
	double bo = 1e9, bn = 1e9;
	size_t total = 0;
	int i, t;

	/* The shape FreezeInternals actually produces: mostly small register
	 * blocks and structs, with the occasional large memory region. */
	for (i = 0; i < NBLOCKS; i++)
	{
		rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
		sizes[i] = (i % 97 == 0) ? (int)(rs % 40000u) + 1000
		                         : (int)(rs % 256u) + 1;
		total += (size_t)sizes[i];
	}
	memset(scratch, 0x5a, sizeof(scratch));

	for (t = 0; t < trials; t++)
	{
		double a = bench_old_run(reps, NBLOCKS, sizes, scratch);
		double b = run_new(reps);
		if (a < bo) bo = a;
		if (b < bn) bn = b;
	}

	printf("state           : %lu bytes over %d blocks\n",
	       (unsigned long)total, NBLOCKS);
	printf("C++ vector<u8>& : %.4f s\n", bo);
	printf("C   struct      : %.4f s\n", bn);
	printf("%s: %.2fx\n", bn <= bo ? "PASS" : "FAIL (C is slower)", bo / bn);
	return bn <= bo ? 0 : 1;
}
