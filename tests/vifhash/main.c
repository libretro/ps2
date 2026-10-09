/* The VIF dynarec's block hash (Vif_HashBucket.h).
 *
 *   - after a reset every bucket is empty: no key is found, the all-zero
 *     key included, and every bucket shares the one sentinel;
 *   - blocks added are found by their key, chains grow in order, and a
 *     key that was not added is not found;
 *   - the shared sentinel is never written: it is all-zero after every
 *     add, so an empty bucket stays empty;
 *   - a reset frees every chain and leaves the buckets empty again, run
 *     after run (LSan sees no chain left behind), and a clear frees them
 *     for good.
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Vif_HashBucket.h"

static vif_hash_bucket_t s_h;
static int s_failures;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		printf("  FAIL: %s\n", what);
		s_failures++;
	}
}

static nVifBlock make(u32 hash, u32 key0, u32 key1, u32 value)
{
	nVifBlock b;
	memset(&b, 0, sizeof(b));
	b.k.hash_key = (u16)hash;
	b.k.key0     = key0;
	b.k.key1     = key1;
	b.k.value    = value;
	return b;
}

#define KEY(k0, k1) ((u64)(k0) | ((u64)(k1) << 32))

static int sentinel_clear(void)
{
	static const nVifBlock zero;
	return !memcmp(&s_h.empty, &zero, sizeof(zero));
}

int main(void)
{
	int run, i, shared = 0;
	unsigned seed = 1;

	vif_hash_reset(&s_h);
	for (i = 0; i < VIF_HASH_SIZE; i++)
		shared += s_h.bucket[i] == &s_h.empty;
	check(shared == VIF_HASH_SIZE, "a reset leaves a bucket with a sentinel of its own");
	check(!vif_hash_find(&s_h, 0, 0), "the all-zero key is found in an empty hash");

	for (run = 0; run < 4; run++)
	{
		const int n = 3000;
		const unsigned run_seed = seed;
		int missing = 0, wrong = 0;
		for (i = 0; i < n; i++)
		{
			nVifBlock b;
			seed = seed * 1103515245u + 12345u;
			/* A few buckets get long chains, the rest one block. */
			b = make((i % 7) ? (seed >> 8) & 0xFFFF : 0x0101, (u32)i, (u32)run, (u32)i + 1);
			vif_hash_add(&s_h, &b);
		}
		check(sentinel_clear(), "an add wrote the shared sentinel");
		/* Look every block of this run up again by key, replaying the
		 * hashes from where the run started. */
		{
			unsigned s2 = run_seed;
			for (i = 0; i < n; i++)
			{
				nVifBlock *f;
				u32 hash;
				s2 = s2 * 1103515245u + 12345u;
				hash = (i % 7) ? (s2 >> 8) & 0xFFFF : 0x0101;
				f = vif_hash_find(&s_h, hash, KEY(i, run));
				if (!f)
					missing++;
				else if (f->k.value != (u32)i + 1)
					wrong++;
			}
		}
		check(!missing, "an added block is not found");
		check(!wrong, "a key finds another block");
		check(!vif_hash_find(&s_h, 0x0101, KEY(n + 5, run)), "a key never added is found");
		check(vif_hash_bucket_size(&s_h, 0x0101) >= (u32)(n / 7), "a long chain lost blocks");

		vif_hash_reset(&s_h);
		shared = 0;
		for (i = 0; i < VIF_HASH_SIZE; i++)
			shared += s_h.bucket[i] == &s_h.empty;
		check(shared == VIF_HASH_SIZE, "a reset leaves a chain in a bucket");
		check(!vif_hash_find(&s_h, 0x0101, KEY(0, run)), "a block is found after a reset");
	}

	vif_hash_clear(&s_h);
	for (i = 0; i < VIF_HASH_SIZE; i++)
		if (s_h.bucket[i])
		{
			check(0, "a clear leaves a bucket");
			break;
		}

	printf(s_failures ? "vif hash: FAILED (%d)\n" : "vif hash: ok\n", s_failures);
	return s_failures != 0;
}
