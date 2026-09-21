/* The IPU inverse DCT against a floating-point model and across tiers.
 *
 * ipu_idct_copy() and ipu_idct_add() share IDCT_Block, whose column pass
 * has three spellings: pmaddwd from SSE4.1 up, widening multiplies on
 * aarch64, and scalar code below both. Nothing in the tree checks that
 * they agree, and the scalar one is dead on any build with SSE4.1 or
 * NEON, so it can drift unnoticed. Running every tier against one pinned
 * hash is what holds them together -- an -msse2 build takes the scalar
 * column pass, -msse4.1 and aarch64 take a vector one each, and a hash
 * that differs between them is the finding.
 *
 * A pinned hash only says the output did not move. To catch a mistake
 * that was always there, the blocks also go through a double-precision
 * IDCT written from the transform's definition, and the integer result
 * has to stay within a few steps of it. That model shares no code with
 * either path, so a structural error in both is still caught.
 *
 * The two kernels also promise to leave the block cleared, which the
 * decoder relies on to avoid memsetting between blocks; that is checked
 * on every iteration rather than assumed.
 *
 *   ./build.sh            check against the pinned hashes
 *   ./build.sh --print    print hashes to re-pin, when output should change
 */
#include "IPU/ipu_idct.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		*h = (*h ^ (uint64_t)b[i]) * 1099511628211ull;
}

static uint32_t rs = 0x9e3779b9u;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* Coefficient blocks shaped like what the slice decoders actually hand
 * over: mostly a DC term with a sparse low-frequency tail, plus the
 * degenerate cases at both ends. The all-zero row is a fast path inside
 * the row pass, and full-scale coefficients are what drive the column
 * output to the +-3826 the header warns about. */
static void fill_block(s16 *b, int k)
{
	memset(b, 0, 64 * sizeof(s16));

	switch (k & 15)
	{
	case 0: /* all zero: every row takes the fast path */
		break;
	case 1: /* DC only */
		b[0] = (s16)(rnd() & 0x7ff);
		break;
	case 2: /* negative DC only */
		b[0] = (s16)(-(int)(rnd() & 0x7ff));
		break;
	case 3: /* full scale everywhere: the worst case for the column pass */
		for (int i = 0; i < 64; i++)
			b[i] = (s16)((rnd() & 1) ? 32767 : -32768);
		break;
	case 4: /* one row live, the other seven on the fast path. Kept small
		 * enough that the row pass stays inside s16: a row of large
		 * high-frequency coefficients wraps on the way out of it, and
		 * that regime belongs to case 3, which the model skips. */
		for (int i = 0; i < 8; i++)
			b[8 * 3 + i] = (s16)((int)(rnd() & 0x3ff) - 512);
		break;
	case 5: /* highest frequency only */
		b[63] = (s16)(rnd() & 0x7ff);
		break;
	default: /* DC plus a sparse low-frequency tail */
		b[0] = (s16)((int)(rnd() & 0x3ff) - 512);
		for (int i = 0; i < 10; i++)
		{
			const int p = (int)(rnd() % 64);
			b[p] = (s16)((int)(rnd() & 0x1ff) - 256);
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* What the transform is supposed to be.                                */
/* ------------------------------------------------------------------ */

/* Separable 8x8 IDCT straight from the definition, in the same scale the
 * kernel works in: its row pass shifts up by 11 and down by 8, the column
 * pass up by 11 and down by 17, against W constants carrying 2048*sqrt2,
 * which lands on the orthonormal transform exactly. A DC of D comes out as
 * D/8 either way. */
static void model_idct(const s16 *in, double *out)
{
	static double c[8][8];
	static bool built = false;
	if (!built)
	{
		built = true;
		for (int x = 0; x < 8; x++)
			for (int u = 0; u < 8; u++)
				c[x][u] = (u == 0 ? sqrt(0.125) : 0.5)
				        * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
	}

	/* The block does not arrive in raster frequency order. The scan tables
	 * fold in mpeg2dec's IDCT permutation -- make_scan_pack() builds each
	 * entry as ((j & 0x36) >> 1) | ((j & 0x09) << 2), which rotates the
	 * three row bits and the three column bits the same way -- so the
	 * coefficient for frequency (u, v) sits at [8 * perm[v] + perm[u]].
	 * Reading the block as raster makes every odd frequency land on an
	 * even one, and the kernel then looks mirror-symmetric when it is not. */
	static const int perm[8] = { 0, 4, 1, 5, 2, 6, 3, 7 };

	double tmp[64];
	for (int v = 0; v < 8; v++)
		for (int x = 0; x < 8; x++)
		{
			double s = 0.0;
			for (int u = 0; u < 8; u++)
				s += c[x][u] * (double)in[8 * perm[v] + perm[u]];
			tmp[8 * v + x] = s;
		}
	for (int x = 0; x < 8; x++)
		for (int y = 0; y < 8; y++)
		{
			double s = 0.0;
			for (int v = 0; v < 8; v++)
				s += c[y][v] * tmp[8 * v + x];
			out[8 * y + x] = s;
		}
}

/* ------------------------------------------------------------------ */

static long mismatches;
static long compared;
static double worst_err;
static int worst_iter = -1;

static void fail(const char *what, int iter)
{
	if (mismatches < 8)
		printf("  FAIL %s, iteration %d\n", what, iter);
	mismatches++;
}

int main(int argc, char **argv)
{
	const int iters = argc > 1 ? atoi(argv[1]) : 20000;
	const int print = (argc > 2 && strcmp(argv[2], "--print") == 0);
	uint64_t h_copy = hash_init(), h_add = hash_init();

	/* Pinned from the tree as it stands, at the default 20000 blocks. */
	const uint64_t want_copy = 0x324c0d3eaa73a8b8ull;
	const uint64_t want_add  = 0x6b259805f68a161aull;

	/* The model and the kernel round differently at every stage, so they
	 * never agree exactly. They do have to stay close: anything past a
	 * couple of steps is a structural difference, not rounding. Blocks
	 * driven to full scale are excluded from the bound -- they overflow
	 * the 16-bit intermediate on purpose, which the model does not
	 * model. */
	const double err_limit = 2.0;

	printf("tier: ");
#if defined(__AVX2__)
	printf("avx2 (vector column pass)\n");
#elif defined(__SSE4_1__)
	printf("sse4.1 (vector column pass)\n");
#elif defined(__SSE2__) || defined(__x86_64__)
	printf("sse2 (scalar column pass)\n");
#elif defined(_M_ARM64) || defined(__aarch64__)
	printf("aarch64 (NEON column pass)\n");
#else
	printf("scalar\n");
#endif

	for (int k = 0; k < iters; k++)
	{
		/* Both kernels load and store the block a row at a time with
		 * aligned moves, and ipu_idct_add does the same to dest. */
		alignas(16) s16 block[64];
		alignas(16) s16 src[64];
		alignas(16) u8  dst8[8 * 16];
		alignas(16) s16 dst16[8 * 16];
		double ref[64];

		fill_block(src, k);

		/* ---- ipu_idct_copy: clamped to bytes, block left cleared */
		memcpy(block, src, sizeof(block));
		memset(dst8, 0xcd, sizeof(dst8));
		ipu_idct_copy(block, dst8, 16);
		compared++;

		for (int i = 0; i < 64; i++)
			if (block[i] != 0)
			{
				fail("ipu_idct_copy left the block dirty", k);
				break;
			}

		/* Against the model, on the rows the kernel actually clamps. */
		if ((k & 15) != 3)
		{
			model_idct(src, ref);
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
				{
					double m = ref[8 * y + x];
					if (m < 0.0)   m = 0.0;
					if (m > 255.0) m = 255.0;
					const double e = fabs((double)dst8[y * 16 + x] - m);
					if (e > worst_err) { worst_err = e; worst_iter = k; }
					if (e > err_limit)
					{
						fail("ipu_idct_copy strayed from the model", k);
						y = 8;
						break;
					}
				}
		}
		hash_bytes(&h_copy, dst8, sizeof(dst8));

		/* ---- ipu_idct_add, full path: last != 129 */
		memcpy(block, src, sizeof(block));
		memset(dst16, 0xcd, sizeof(dst16));
		ipu_idct_add(0, block, dst16, 16);
		compared++;

		for (int i = 0; i < 64; i++)
			if (block[i] != 0)
			{
				fail("ipu_idct_add left the block dirty", k);
				break;
			}
		hash_bytes(&h_add, dst16, sizeof(dst16));

		/* ---- ipu_idct_add, DC shortcut: last == 129 and block[0] & 7
		 * not 4. Forced rather than hoped for, since a random block
		 * lands on it about one time in eight. */
		memcpy(block, src, sizeof(block));
		block[0] = (s16)((block[0] & ~7) | ((k & 1) ? 5 : 2));
		memset(dst16, 0xcd, sizeof(dst16));
		ipu_idct_add(129, block, dst16, 16);
		compared++;

		if (block[0] != 0 || block[63] != 0)
			fail("ipu_idct_add DC path did not clear block[0]/[63]", k);
		{
			/* Every quadword of every row is the same replicated DC. */
			const s16 dc = dst16[0];
			for (int y = 0; y < 8; y++)
				for (int x = 0; x < 8; x++)
					if (dst16[y * 16 + x] != dc)
					{
						fail("ipu_idct_add DC path did not splat", k);
						y = 8;
						break;
					}
		}
		hash_bytes(&h_add, dst16, sizeof(dst16));

		/* ---- and the other side of that test: last == 129 with the
		 * low bits equal to 4 runs the full transform. */
		memcpy(block, src, sizeof(block));
		block[0] = (s16)((block[0] & ~7) | 4);
		memset(dst16, 0xcd, sizeof(dst16));
		ipu_idct_add(129, block, dst16, 16);
		compared++;
		hash_bytes(&h_add, dst16, sizeof(dst16));
	}

	if (print)
	{
		printf("  { %016llx, %016llx }\n",
		       (unsigned long long)h_copy, (unsigned long long)h_add);
		return 0;
	}

	printf("  idct_copy %016llx%s\n", (unsigned long long)h_copy,
	       want_copy == 0 ? "  (unpinned)" :
	       (h_copy == want_copy ? "  ok" : "  UNEXPECTED"));
	printf("  idct_add  %016llx%s\n", (unsigned long long)h_add,
	       want_add == 0 ? "  (unpinned)" :
	       (h_add == want_add ? "  ok" : "  UNEXPECTED"));
	printf("  worst deviation from the model: %.3f (iteration %d, limit %.1f)\n",
	       worst_err, worst_iter, err_limit);

	{
		const int bad = (mismatches != 0)
		              || (want_copy && h_copy != want_copy)
		              || (want_add && h_add != want_add);
		printf("%s: IPU inverse DCT, %ld checks, %ld failures\n",
		       bad ? "FAIL" : "PASS", compared, mismatches);
		return bad;
	}
}
