/* gs_stable_group (GSStableGroup.h), which batches the texture cache's
 * copies by source and filter, on random lists:
 *   - the result is a permutation of the input;
 *   - equal items are contiguous;
 *   - groups come in the order their first item appears, and items of a
 *     group in the order they came;
 *   - the same keys behind different pointers - laid out in memory the
 *     other way round - group into the same order;
 * and the same for items larger than the routine's stack buffer. C89.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/Common/GSStableGroup.h"

struct item
{
	const void* src;
	int         linear;
	int         seq;
};

struct big_item
{
	const void* src;
	int         linear;
	int         seq;
	char        pad[200];
};

static int same_item(const void* a, const void* b)
{
	const struct item* l = (const struct item*)a;
	const struct item* r = (const struct item*)b;
	return l->src == r->src && l->linear == r->linear;
}

static int same_big(const void* a, const void* b)
{
	const struct big_item* l = (const struct big_item*)a;
	const struct big_item* r = (const struct big_item*)b;
	return l->src == r->src && l->linear == r->linear;
}

static unsigned long s_rng = 1;
static unsigned rnd(unsigned n)
{
	s_rng = s_rng * 1103515245UL + 12345UL;
	return (unsigned)((s_rng >> 16) & 0x7FFF) % n;
}

static int failures;

/* keys[i] is the source index and filter of item i; seq_out gets the
 * order the items come out in. 0 if the grouping is wrong. */
static int check(const int* keys, int n, const char* const* sources, int big, int* seq_out)
{
	static struct item     a[256];
	static struct big_item b[256];
	static int seen[256];
	int i, j;

	for (i = 0; i < n; i++)
	{
		a[i].src = b[i].src = sources[keys[i] >> 1];
		a[i].linear = b[i].linear = keys[i] & 1;
		a[i].seq = b[i].seq = i;
		memset(b[i].pad, i, sizeof(b[i].pad));
	}
	if (big)
		gs_stable_group(b, (size_t)n, sizeof(b[0]), same_big);
	else
		gs_stable_group(a, (size_t)n, sizeof(a[0]), same_item);

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < n; i++)
	{
		const int s = big ? b[i].seq : a[i].seq;
		if (s < 0 || s >= n || seen[s]++)
			return 0;
		if (big && b[i].pad[0] != (char)s)
			return 0;
		seq_out[i] = s;
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
		{
			const int ki = keys[seq_out[i]], kj = keys[seq_out[j]];
			/* Contiguous: an item equal to i past an unequal one. */
			if (ki == kj && j > i + 1 && keys[seq_out[j - 1]] != ki)
				return 0;
			/* Stable within a group. */
			if (ki == kj && seq_out[i] > seq_out[j])
				return 0;
		}
	/* Groups in order of first appearance: the first items of the
	 * groups, read in output order, come in increasing input order. */
	{
		int last_first = -1;
		for (i = 0; i < n; i++)
			if (i == 0 || keys[seq_out[i]] != keys[seq_out[i - 1]])
			{
				if (seq_out[i] < last_first)
					return 0;
				last_first = seq_out[i];
			}
	}
	return 1;
}

int main(void)
{
	static char pool[64];
	const char* up[32];
	const char* down[32];
	static int keys[256], first[256], second[256];
	int round, i;

	for (i = 0; i < 32; i++)
	{
		up[i]   = pool + i;
		down[i] = pool + 31 - i;
	}
	for (round = 0; round < 2000; round++)
	{
		const int n = 1 + (int)rnd(200);
		const int distinct = 1 + (int)rnd(32);
		const int big = round & 1;
		for (i = 0; i < n; i++)
			keys[i] = (int)(rnd((unsigned)distinct) << 1) | (int)rnd(2);
		if (!check(keys, n, up, big, first) || !check(keys, n, down, big, second))
		{
			if (failures++ < 5)
				printf("  FAIL: round %d (%d items, %s): not grouped in order\n",
					round, n, big ? "large" : "small");
			continue;
		}
		if (memcmp(first, second, (size_t)n * sizeof(int)))
		{
			if (failures++ < 5)
				printf("  FAIL: round %d: the order follows where the sources sit\n", round);
		}
	}
	printf(failures ? "gsgroup: FAILED (%d)\n" : "gsgroup: ok, 2000 lists\n", failures);
	return failures != 0;
}
