/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Some of the functions in this file are based on the mpeg2dec library,
 *
 *  Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org>
 *  Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *  Modified by Florin for PCSX2 emu
 *
 *  under the GPL license. However, they have been heavily rewritten for PCSX2 usage.
 *  The original author's copyright statement is included above for completeness sake.
 */

#include <string.h>

#include "../../common/VectorIntrin.h"

#include "ipu_idct.h"

#define W1 2841 /* 2048*sqrt (2)*cos (1*pi/16) */
#define W2 2676 /* 2048*sqrt (2)*cos (2*pi/16) */
#define W3 2408 /* 2048*sqrt (2)*cos (3*pi/16) */
#define W5 1609 /* 2048*sqrt (2)*cos (5*pi/16) */
#define W6 1108 /* 2048*sqrt (2)*cos (6*pi/16) */
#define W7 565  /* 2048*sqrt (2)*cos (7*pi/16) */

/*
 * In legal streams, the IDCT output should be between -384 and +384.
 * In corrupted streams, it is possible to force the IDCT output to go
 * to +-3826 - this is the worst case for a column IDCT where the
 * column inputs are 16-bit values.
 */

/* One quadword moved, zeroed or splatted. The decoder walks the block a row
 * at a time and these are the three things it does to a row, so they are
 * spelled once here per target rather than at each use. */
#if _M_SSE >= 0x200
#define QW_COPY(dst, src) \
	_mm_store_si128((__m128i *)(dst), _mm_load_si128((const __m128i *)(src)))
#define QW_ZERO(dst) \
	_mm_store_si128((__m128i *)(dst), _mm_setzero_si128())
typedef __m128i idct_qw;
#define QW_DUP32(v) _mm_set1_epi32((int)(v))
#define QW_STORE(dst, v) _mm_store_si128((__m128i *)(dst), (v))
#elif defined(_M_ARM64) || defined(__aarch64__)
#define QW_COPY(dst, src) \
	vst1q_u8((u8 *)(dst), vld1q_u8((const u8 *)(src)))
#define QW_ZERO(dst) \
	vst1q_u8((u8 *)(dst), vmovq_n_u8(0))
typedef uint32x4_t idct_qw;
#define QW_DUP32(v) vdupq_n_u32((u32)(v))
#define QW_STORE(dst, v) vst1q_u32((uint32_t *)(dst), (v))
#else
typedef struct idct_qw { u32 w[4]; } idct_qw;
#define QW_COPY(dst, src) memcpy((dst), (src), 16)
#define QW_ZERO(dst) memset((dst), 0, 16)
#define QW_DUP32(v) idct_qw_dup32((u32)(v))
#define QW_STORE(dst, v) memcpy((dst), (v).w, 16)

static idct_qw idct_qw_dup32(u32 v)
{
	idct_qw q;
	q.w[0] = q.w[1] = q.w[2] = q.w[3] = v;
	return q;
}
#endif

#if _M_SSE >= 0x401
static PCSX2_INLINE __m128i pair_const(int lo, int hi)
{
	return _mm_set1_epi32((int)((unsigned)(hi & 0xffff) << 16) | (unsigned)(lo & 0xffff));
}
#else
/* Writes the pair t0, t1 from the product pair w0, w1 against d0, d1. A
 * macro rather than a function so the two results stay in registers without
 * depending on the compiler to see through a pair of out-pointers. The call
 * sites keep their outputs distinct from their inputs. */
#define BUTTERFLY(t0, t1, w0, w1, d0, d1)             \
	do {                                          \
		const int bf_tmp = (w0) * ((d0) + (d1)); \
		(t0) = bf_tmp + ((w1) - (w0)) * (d1); \
		(t1) = bf_tmp - ((w1) + (w0)) * (d0); \
	} while (0)
#endif

/* __fi rather than a bare static: the two callers below are the only ones,
 * and both want this inlined. Spelled __fi because mingw's C-mode
 * __forceinline carries a storage class, which static then collides with. */
static __fi void IDCT_Block(s16 *block)
{
	int i;

	/* ---- row pass: unchanged scalar code, keeps its all-zero fast path */
	for (i = 0; i < 8; i++)
	{
		s16 * const rblock = block + 8 * i;
		int a0, a1, a2, a3;
		int b0, b1, b2, b3;

		if (!(rblock[1] | ((s32 *)rblock)[1] | ((s32 *)rblock)[2] |
				((s32 *)rblock)[3]))
		{
			u32 tmp = (u16)(rblock[0] << 3);
			tmp |= tmp << 16;
			((s32 *)rblock)[0] = tmp;
			((s32 *)rblock)[1] = tmp;
			((s32 *)rblock)[2] = tmp;
			((s32 *)rblock)[3] = tmp;
			continue;
		}

		{
			const int d0 = (rblock[0] << 11) + 128;
			const int d1 = rblock[1];
			const int d2 = rblock[2] << 11;
			const int d3 = rblock[3];
			const int t0 = d0 + d2;
			const int t1 = d0 - d2;
			const int t2 = W6 * d3 + W2 * d1;
			const int t3 = W6 * d1 - W2 * d3;
			a0 = t0 + t2; a1 = t1 + t3; a2 = t1 - t3; a3 = t0 - t2;
		}
		{
			const int d0 = rblock[4], d1 = rblock[5], d2 = rblock[6], d3 = rblock[7];
			int t0 = W7 * d3 + W1 * d0;
			int t1 = W7 * d0 - W1 * d3;
			const int t2 = W3 * d1 + W5 * d2;
			const int t3 = W3 * d2 - W5 * d1;
			b0 = t0 + t2; b3 = t1 + t3;
			t0 -= t2; t1 -= t3;
			/* The 181 products run to about 8.8e10 once the
			 * coefficients approach full scale, which a stream can
			 * make them do. Wrapping is what the result is defined
			 * as -- the vector column pass below reaches the same
			 * place through _mm_mullo_epi32, which wraps by
			 * definition -- so take the product in the unsigned
			 * domain, where it carries the same bits without the
			 * signed overflow. */
			b1 = (s32)((u32)(t0 + t1) * 181u) >> 8;
			b2 = (s32)((u32)(t0 - t1) * 181u) >> 8;
		}
		rblock[0] = (a0 + b0) >> 8;
		rblock[1] = (a1 + b1) >> 8;
		rblock[2] = (a2 + b2) >> 8;
		rblock[3] = (a3 + b3) >> 8;
		rblock[4] = (a3 - b3) >> 8;
		rblock[5] = (a2 - b2) >> 8;
		rblock[6] = (a1 - b1) >> 8;
		rblock[7] = (a0 - b0) >> 8;
	}

#if _M_SSE >= 0x401
	/* ---- column pass: eight columns in parallel.
	 *
	 * Each row of the block is one contiguous vector of eight s16, and
	 * column k's inputs are lane k of rows 0..7, so no transpose is
	 * needed.  The butterflies expand to plain product pairs
	 *   t2 = W6*d3 + W2*d1,  t3 = W6*d1 - W2*d3
	 * which is exactly what pmaddwd computes from interleaved 16-bit
	 * operands - one instruction per pair instead of a 32-bit multiply
	 * chain, which is where this beats what the compiler emits from the
	 * scalar form. */
	{
	const __m128i r0 = _mm_load_si128((const __m128i *)(block + 8 * 0));
	const __m128i r1 = _mm_load_si128((const __m128i *)(block + 8 * 1));
	const __m128i r2 = _mm_load_si128((const __m128i *)(block + 8 * 2));
	const __m128i r3 = _mm_load_si128((const __m128i *)(block + 8 * 3));
	const __m128i r4 = _mm_load_si128((const __m128i *)(block + 8 * 4));
	const __m128i r5 = _mm_load_si128((const __m128i *)(block + 8 * 5));
	const __m128i r6 = _mm_load_si128((const __m128i *)(block + 8 * 6));
	const __m128i r7 = _mm_load_si128((const __m128i *)(block + 8 * 7));

	const __m128i k_w6w2 = pair_const(W6, W2);
	const __m128i k_nw2w6 = pair_const(-W2, W6);
	const __m128i k_w7w1 = pair_const(W7, W1);
	const __m128i k_nw1w7 = pair_const(-W1, W7);
	const __m128i k_w3w5 = pair_const(W3, W5);
	const __m128i k_nw5w3 = pair_const(-W5, W3);
	const __m128i k_round = _mm_set1_epi32(65536);
	const __m128i k_181 = _mm_set1_epi32(181);

	__m128i lo[8];
	int h;
	for (h = 0; h < 2; h++)
	{
		const __m128i i31 = h ? _mm_unpackhi_epi16(r3, r1) : _mm_unpacklo_epi16(r3, r1);
		const __m128i i74 = h ? _mm_unpackhi_epi16(r7, r4) : _mm_unpacklo_epi16(r7, r4);
		const __m128i i56 = h ? _mm_unpackhi_epi16(r5, r6) : _mm_unpacklo_epi16(r5, r6);

		const __m128i e0 = h ? _mm_cvtepi16_epi32(_mm_srli_si128(r0, 8)) : _mm_cvtepi16_epi32(r0);
		const __m128i e2 = h ? _mm_cvtepi16_epi32(_mm_srli_si128(r2, 8)) : _mm_cvtepi16_epi32(r2);

		const __m128i d0 = _mm_add_epi32(_mm_slli_epi32(e0, 11), k_round);
		const __m128i d2 = _mm_slli_epi32(e2, 11);
		const __m128i t0 = _mm_add_epi32(d0, d2);
		const __m128i t1 = _mm_sub_epi32(d0, d2);
		const __m128i t2 = _mm_madd_epi16(i31, k_w6w2);
		const __m128i t3 = _mm_madd_epi16(i31, k_nw2w6);

		const __m128i a0 = _mm_add_epi32(t0, t2);
		const __m128i a1 = _mm_add_epi32(t1, t3);
		const __m128i a2 = _mm_sub_epi32(t1, t3);
		const __m128i a3 = _mm_sub_epi32(t0, t2);

		const __m128i u0 = _mm_madd_epi16(i74, k_w7w1);
		const __m128i u1 = _mm_madd_epi16(i74, k_nw1w7);
		const __m128i u2 = _mm_madd_epi16(i56, k_w3w5);
		const __m128i u3 = _mm_madd_epi16(i56, k_nw5w3);

		const __m128i b0 = _mm_add_epi32(u0, u2);
		const __m128i b3 = _mm_add_epi32(u1, u3);
		const __m128i s0 = _mm_srai_epi32(_mm_sub_epi32(u0, u2), 8);
		const __m128i s1 = _mm_srai_epi32(_mm_sub_epi32(u1, u3), 8);
		const __m128i b1 = _mm_mullo_epi32(_mm_add_epi32(s0, s1), k_181);
		const __m128i b2 = _mm_mullo_epi32(_mm_sub_epi32(s0, s1), k_181);

		const __m128i o0 = _mm_srai_epi32(_mm_add_epi32(a0, b0), 17);
		const __m128i o1 = _mm_srai_epi32(_mm_add_epi32(a1, b1), 17);
		const __m128i o2 = _mm_srai_epi32(_mm_add_epi32(a2, b2), 17);
		const __m128i o3 = _mm_srai_epi32(_mm_add_epi32(a3, b3), 17);
		const __m128i o4 = _mm_srai_epi32(_mm_sub_epi32(a3, b3), 17);
		const __m128i o5 = _mm_srai_epi32(_mm_sub_epi32(a2, b2), 17);
		const __m128i o6 = _mm_srai_epi32(_mm_sub_epi32(a1, b1), 17);
		const __m128i o7 = _mm_srai_epi32(_mm_sub_epi32(a0, b0), 17);
		if (h == 0)
		{
			lo[0] = o0; lo[1] = o1; lo[2] = o2; lo[3] = o3;
			lo[4] = o4; lo[5] = o5; lo[6] = o6; lo[7] = o7;
		}
		else
		{
			_mm_store_si128((__m128i *)(block + 8 * 0), _mm_packs_epi32(lo[0], o0));
			_mm_store_si128((__m128i *)(block + 8 * 1), _mm_packs_epi32(lo[1], o1));
			_mm_store_si128((__m128i *)(block + 8 * 2), _mm_packs_epi32(lo[2], o2));
			_mm_store_si128((__m128i *)(block + 8 * 3), _mm_packs_epi32(lo[3], o3));
			_mm_store_si128((__m128i *)(block + 8 * 4), _mm_packs_epi32(lo[4], o4));
			_mm_store_si128((__m128i *)(block + 8 * 5), _mm_packs_epi32(lo[5], o5));
			_mm_store_si128((__m128i *)(block + 8 * 6), _mm_packs_epi32(lo[6], o6));
			_mm_store_si128((__m128i *)(block + 8 * 7), _mm_packs_epi32(lo[7], o7));
		}
	}
	}
#else
	for (i = 0; i < 8; i++)
	{
		s16 * const cblock = block + i;
		int a0, a1, a2, a3;
		int b0, b1, b2, b3;

		{
			const int d0 = (cblock[8 * 0] << 11) + 65536;
			const int d1 = cblock[8 * 1];
			const int d2 = cblock[8 * 2] << 11;
			const int d3 = cblock[8 * 3];
			const int t0 = d0 + d2;
			const int t1 = d0 - d2;
			int t2;
			int t3;
			BUTTERFLY(t2, t3, W6, W2, d3, d1);
			a0 = t0 + t2;
			a1 = t1 + t3;
			a2 = t1 - t3;
			a3 = t0 - t2;
		}

		{
			const int d0 = cblock[8 * 4];
			const int d1 = cblock[8 * 5];
			const int d2 = cblock[8 * 6];
			const int d3 = cblock[8 * 7];
			int t0, t1, t2, t3;
			BUTTERFLY(t0, t1, W7, W1, d3, d0);
			BUTTERFLY(t2, t3, W3, W5, d1, d2);
			b0 = t0 + t2;
			b3 = t1 + t3;
			t0 = (t0 - t2) >> 8;
			t1 = (t1 - t3) >> 8;
			b1 = (t0 + t1) * 181;
			b2 = (t0 - t1) * 181;
		}

		cblock[8 * 0] = (a0 + b0) >> 17;
		cblock[8 * 1] = (a1 + b1) >> 17;
		cblock[8 * 2] = (a2 + b2) >> 17;
		cblock[8 * 3] = (a3 + b3) >> 17;
		cblock[8 * 4] = (a3 - b3) >> 17;
		cblock[8 * 5] = (a2 - b2) >> 17;
		cblock[8 * 6] = (a1 - b1) >> 17;
		cblock[8 * 7] = (a0 - b0) >> 17;
	}
#endif
}

void ipu_idct_copy(s16 *block, u8 *dest, const int stride)
{
	int i;

	IDCT_Block(block);

	/* Saturating pack does what the clip table did - negatives to 0,
	 * above 255 to 255 - for a whole row at a time, and does it for
	 * every input rather than the table's -384..639 window.  That
	 * window was not a bound on the data: the comment above IDCT_Block
	 * records that a corrupted stream can drive a column output to
	 * +-3826, and indexing a 1024-byte table with that read up to three
	 * kilobytes past its end. */
#if _M_SSE >= 0x200
	for (i = 0; i < 8; i++)
	{
		const __m128i row = _mm_load_si128((const __m128i *)block);
		_mm_storel_epi64((__m128i *)dest, _mm_packus_epi16(row, row));
		QW_ZERO(block);

		dest += stride;
		block += 8;
	}
#else
	/* Same clamp, written out: targets without the x86 intrinsics still
	 * must not index a table with an out-of-range IDCT output. */
	for (i = 0; i < 8; i++)
	{
		int j;
		for (j = 0; j < 8; j++)
		{
			const int v = block[j];
			dest[j] = (u8)((v < 0) ? 0 : ((v > 255) ? 255 : v));
			block[j] = 0;
		}

		dest += stride;
		block += 8;
	}
#endif
}

void ipu_idct_add(const int last, s16 *block, s16 *dest, const int stride)
{
	int i;

	/* on the IPU, stride is always assured to be multiples of QWC
	 * (bottom 3 bits are 0). */
	if (last != 129 || (block[0] & 7) == 4)
	{
		IDCT_Block(block);

		for (i = 0; i < 8; i++)
		{
			QW_COPY(dest, block);
			QW_ZERO(block);

			dest += stride;
			block += 8;
		}
	}
	else
	{
		const u16 DC       = (u16)(((s32)block[0] + 4) >> 3);
		const idct_qw dc16 = QW_DUP32((u32)DC | ((u32)DC << 16));
		block[0] = block[63] = 0;

		for (i = 0; i < 8; ++i)
			QW_STORE(dest + (stride * i), dc16);
	}
}
