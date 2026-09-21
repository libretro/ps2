/* The IPU bitstream window readers against a bit-at-a-time reference.
 *
 * Everything in ipu_bitstream.c reads a run of bits out of a 32-byte
 * window at an arbitrary bit position, by loading a machine word and
 * shifting. The model here instead walks the window one bit at a time,
 * which is slow and obviously correct, and shares nothing with the shifting
 * version -- so a masking or endianness mistake shows up rather than being
 * reproduced.
 *
 * Alignment is swept exhaustively rather than sampled. A real stream only
 * ever visits the bit positions its own symbol lengths happen to produce,
 * so it is a far weaker test of these functions than every position in
 * 0..127 against random window contents.
 *
 *   ./bitstream_build.sh            check against the pinned hashes
 *   ./bitstream_build.sh --print    print hashes to re-pin
 */
#include "IPU/ipu_bitstream.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	for (size_t i = 0; i < n; i++)
		*h = (*h ^ (uint64_t)b[i]) * 1099511628211ull;
}

static uint32_t rs = 0x243f6a88u;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* ------------------------------------------------------------------ */
/* What "read n bits at position bp" means, one bit at a time.          */
/* ------------------------------------------------------------------ */

/* Bit 0 of the stream is the top bit of byte 0. */
static unsigned bit_at(const u8 *base, u32 bp)
{
	return (base[bp >> 3] >> (7 - (bp & 7))) & 1u;
}

static u32 model_u32(const u8 *base, u32 bp, u32 bits)
{
	u32 v = 0;
	for (u32 i = 0; i < bits; i++)
		v = (v << 1) | bit_at(base, bp + i);
	return v;
}

static s32 model_s32(const u8 *base, u32 bp, u32 bits)
{
	const u32 v = model_u32(base, bp, bits);
	if (bits == 32)
		return (s32)v;
	/* Sign-extend from the top bit that was read. */
	if (v & (1u << (bits - 1)))
		return (s32)(v | ~((1u << bits) - 1u));
	return (s32)v;
}

static void model_copy(const u8 *base, u32 bp, u8 *dst, u32 nbytes)
{
	for (u32 b = 0; b < nbytes; b++)
	{
		u32 byte = 0;
		for (u32 i = 0; i < 8; i++)
			byte = (byte << 1) | bit_at(base, bp + b * 8 + i);
		dst[b] = (u8)byte;
	}
}

/* ------------------------------------------------------------------ */

static long mismatches;
static long compared;

static void fail(const char *what, u32 bp, u32 bits, unsigned long long got,
                 unsigned long long want)
{
	if (mismatches < 10)
		printf("  MISMATCH %s at bp=%u bits=%u: got %llx want %llx\n",
		       what, bp, bits, got, want);
	mismatches++;
}

int main(int argc, char **argv)
{
	const int iters = argc > 1 ? atoi(argv[1]) : 2000;
	const int print = (argc > 2 && strcmp(argv[2], "--print") == 0);
	uint64_t h = hash_init();

	/* Pinned from the tree as it stands, at the default 2000 windows. */
	const uint64_t want_h = 0xebae91756c4490bbull;

	for (int k = 0; k < iters; k++)
	{
		/* The window is two quadwords and the emulator keeps it 16-byte
		 * aligned, so give it that here too. It is a local rather than
		 * a static so ASan puts redzones around it: a read that runs
		 * off either end then faults instead of quietly returning
		 * whatever is next to it. */
		alignas(16) u8 window[32];
		u8 out[8];
		u8 ref[8];

		for (int i = 0; i < 32; i++)
			window[i] = (u8)rnd();

		/* A few windows at the rails: all zero and all ones make the
		 * sign extension and the mask edges unambiguous. */
		if ((k & 63) == 0) memset(window, 0x00, sizeof(window));
		if ((k & 63) == 1) memset(window, 0xff, sizeof(window));

		/* Every bit position the emulator can present, and at each one
		 * every width, rather than a sample of them. A 64-bit read at
		 * bp=127 reaches byte 23, so the window always covers it. */
		for (u32 bp = 0; bp < 128; bp++)
		{
			/* ipu_bits_u32/s32 serve the request out of one 32-bit
			 * word, so a request only means anything while it fits
			 * in that word at or after the bit position. The sweep
			 * runs right up to that boundary at every alignment --
			 * 25 bits at the worst one, 32 on a byte boundary --
			 * which is where an off-by-one in the shift pair shows
			 * up. Past it the functions return zeroes in the low
			 * bits by construction, and the header says so. */
			const u32 max_bits = 32 - (bp & 7);

			for (u32 bits = 1; bits <= max_bits; bits++)
			{
				const u32 gu = ipu_bits_u32(window, bp, bits);
				const u32 wu = model_u32(window, bp, bits);
				compared++;
				if (gu != wu)
					fail("ipu_bits_u32", bp, bits, gu, wu);
				hash_bytes(&h, &gu, sizeof(gu));

				const s32 gs = ipu_bits_s32(window, bp, bits);
				const s32 ws = model_s32(window, bp, bits);
				compared++;
				if (gs != ws)
					fail("ipu_bits_s32", bp, bits,
					     (unsigned long long)(u32)gs,
					     (unsigned long long)(u32)ws);
				hash_bytes(&h, &gs, sizeof(gs));
			}

			memset(out, 0xcd, sizeof(out));
			memset(ref, 0xab, sizeof(ref));
			ipu_bits_copy8(window, bp, out);
			model_copy(window, bp, ref, 1);
			compared++;
			if (memcmp(out, ref, 1) != 0)
				fail("ipu_bits_copy8", bp, 8, out[0], ref[0]);
			hash_bytes(&h, out, 1);

			memset(out, 0xcd, sizeof(out));
			memset(ref, 0xab, sizeof(ref));
			ipu_bits_copy32(window, bp, out);
			model_copy(window, bp, ref, 4);
			compared++;
			if (memcmp(out, ref, 4) != 0)
				fail("ipu_bits_copy32", bp, 32, 0, 0);
			hash_bytes(&h, out, 4);

			memset(out, 0xcd, sizeof(out));
			memset(ref, 0xab, sizeof(ref));
			ipu_bits_copy64(window, bp, out);
			model_copy(window, bp, ref, 8);
			compared++;
			if (memcmp(out, ref, 8) != 0)
				fail("ipu_bits_copy64", bp, 64, 0, 0);
			hash_bytes(&h, out, 8);
		}
	}

	if (print)
	{
		printf("  { %016llx }\n", (unsigned long long)h);
		return 0;
	}

	printf("  bitstream %016llx%s\n", (unsigned long long)h,
	       want_h == 0 ? "  (unpinned)" :
	       (h == want_h ? "  ok" : "  UNEXPECTED"));

	{
		const int bad = (mismatches != 0) || (want_h && h != want_h);
		printf("%s: IPU bitstream window, %ld comparisons, %ld mismatches\n",
		       bad ? "FAIL" : "PASS", compared, mismatches);
		return bad;
	}
}
