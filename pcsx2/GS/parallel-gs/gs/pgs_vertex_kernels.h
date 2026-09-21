/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef PGS_VERTEX_KERNELS_H
#define PGS_VERTEX_KERNELS_H

/* Vertex-kick kernels for paraLLEl-GS, lifted out of GSInterface so the
 * per-vertex sequences can be pinned by tests and read without the vector
 * template layer around them.
 *
 * The kernels take register words, not vector types. Every field a kick
 * writes already sits, in the GS register file, in the order and at the
 * width the shared VertexPosition / VertexAttribute layout wants it, so a
 * kick is a small number of whole-word moves rather than a field-by-field
 * rebuild:
 *
 *   attr[ 0.. 7]  the ST register verbatim         { S, T }
 *   attr[ 8..15]  the RGBAQ register, words swapped{ Q, RGBA }
 *   attr[16..19]  FOG, widened to float
 *   attr[20..23]  the UV register's low word, with the two pad fields
 *                 masked off -- U already sits at bits 0..13 and V at
 *                 16..29, which is where the u16 pair wants them
 *
 * Shape note: written in the C89 form the emitters in this tree use --
 * declarations at the head of each block, no mixed declarations, no early
 * returns in the middle of a body, block comments -- so MSVC's C frontend
 * and its C++ one produce the same straight-line shape.
 */

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define PGS_KICK_INLINE __forceinline
#elif defined(__GNUC__)
#define PGS_KICK_INLINE __inline__ __attribute__((always_inline))
#else
#define PGS_KICK_INLINE
#endif

/* Byte offsets into VertexAttribute. Pinned by static assertions on the
 * C++ side against the shared struct. */
#define PGS_ATTR_ST_OFFSET    0
#define PGS_ATTR_Q_OFFSET     8
#define PGS_ATTR_FOG_OFFSET  16
#define PGS_ATTR_SIZE        24
#define PGS_POS_DEFINED_SIZE 12

/* The register words a kick reads, gathered so the kernels take no
 * C++ types and no vector types. */
struct pgs_kick_regs
{
   uint64_t st;      /* { S, T }, two floats, exactly as attr wants them */
   uint64_t rgbaq;   /* { RGBA, Q } -- attr wants the opposite order     */
   uint32_t uv;      /* U at bits 0..13, V at bits 16..29                */
   uint32_t fog;     /* FOG in the low 8 bits, already extracted         */
   int32_t  ofx;
   int32_t  ofy;
};

/* Position: X and Y are adjacent 16-bit fields in the low word of XYZ or
 * XYZF, so one word carries both. Only the 12 defined bytes are written;
 * the trailing pad keeps whatever it held, as the field-by-field form
 * left it. */
static PGS_KICK_INLINE void pgs_build_position(
      const struct pgs_kick_regs *r, uint32_t xy_word, uint32_t z, void *dst)
{
   uint64_t xy;

   xy  = (uint64_t)(uint32_t)((int32_t)(xy_word & 0xffffu) - r->ofx);
   xy |= (uint64_t)(uint32_t)((int32_t)(xy_word >> 16)     - r->ofy) << 32;

   memcpy((char *)dst, &xy, 8);
   memcpy((char *)dst + 8, &z, 4);
}

/* As above, but also clears the trailing pad, so the 16 bytes that later
 * reach the mapped vertex buffer are all defined. */
static PGS_KICK_INLINE void pgs_build_position_padded(
      const struct pgs_kick_regs *r, uint32_t xy_word, uint32_t z, void *dst)
{
   uint64_t xy;
   uint64_t zw;

   xy  = (uint64_t)(uint32_t)((int32_t)(xy_word & 0xffffu) - r->ofx);
   xy |= (uint64_t)(uint32_t)((int32_t)(xy_word >> 16)     - r->ofy) << 32;
   zw  = (uint64_t)z;

   memcpy((char *)dst, &xy, 8);
   memcpy((char *)dst + 8, &zw, 8);
}

/* Attributes. fog arrives already widened, because the two kick paths
 * take it from different places: XYZ2 from the FOG register, XYZF2 from
 * the vertex word itself. */
static PGS_KICK_INLINE void pgs_build_attribute(
      const struct pgs_kick_regs *r, float fog, void *dst)
{
   char     *p = (char *)dst;
   uint64_t  q;
   uint64_t  tail;
   uint32_t  fog_bits;

   memcpy(p + PGS_ATTR_ST_OFFSET, &r->st, 8);

   q = (r->rgbaq >> 32) | (r->rgbaq << 32);
   memcpy(p + PGS_ATTR_Q_OFFSET, &q, 8);

   memcpy(&fog_bits, &fog, 4);
   tail = (uint64_t)fog_bits | ((uint64_t)(r->uv & 0x3fff3fffu) << 32);
   memcpy(p + PGS_ATTR_FOG_OFFSET, &tail, 8);
}


/* ------------------------------------------------------------------
 * Integer pair helpers.
 *
 * A screen position is two adjacent int32 and a UV pair is two adjacent
 * uint16, so a pair compares in one machine compare and a min/max over
 * three positions is three packed operations rather than twelve scalar
 * ones. The vector form is used where the host has it; the scalar body
 * below is the contract, and the two are pinned against each other by
 * the oracle.
 * ------------------------------------------------------------------ */

/* pminsd/pmaxsd below are SSE4.1, so the gate has to mean SSE4.1. Inside
 * PCSX2 the project's ladder says what the build targets; standalone, the
 * predefined macro does. _M_X64 and _M_IX86_FP are not usable here: both
 * describe an SSE2 baseline, and MSVC defines no SSE4.1 macro of its own.
 * cl.exe emits whatever intrinsic it is given, so there the ladder alone
 * decides; every other compiler checks the target feature, so there the
 * predefined macro has to agree. Where neither says SSE4.1, the scalar
 * bodies below are the contract and are what gets used. */
#if defined(__SSE4_1__) || \
    (defined(_MSC_VER) && !defined(__clang__) && defined(_M_SSE) && _M_SSE >= 0x401)
#include <smmintrin.h>
#define PGS_PAIR_SSE4 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PGS_PAIR_NEON 1
#endif

/* Equality of an int32 pair: one 64-bit compare. */
static PGS_KICK_INLINE int pgs_ivec2_eq(const void *a, const void *b)
{
   uint64_t x;
   uint64_t y;

   memcpy(&x, a, 8);
   memcpy(&y, b, 8);

   return x == y;
}

/* Equality of a uint16 pair: one 32-bit compare. */
static PGS_KICK_INLINE int pgs_u16vec2_eq(const void *a, const void *b)
{
   uint32_t x;
   uint32_t y;

   memcpy(&x, a, 4);
   memcpy(&y, b, 4);

   return x == y;
}

/* Component-wise min and max over two or three int32 pairs.
 *
 * p2 is folded in only when use_p2 is set, which is how the caller
 * distinguishes a triangle from a sprite or a line. lo and hi each
 * receive one pair. */
static PGS_KICK_INLINE void pgs_pair_min_max3(
      const void *p0, const void *p1, const void *p2, int use_p2,
      void *lo, void *hi)
{
#if defined(PGS_PAIR_SSE4)
   __m128i a;
   __m128i b;
   __m128i c;
   __m128i l;
   __m128i h;

   a = _mm_loadl_epi64((const __m128i *)p0);
   b = _mm_loadl_epi64((const __m128i *)p1);
   l = _mm_min_epi32(a, b);
   h = _mm_max_epi32(a, b);

   if (use_p2)
   {
      c = _mm_loadl_epi64((const __m128i *)p2);
      l = _mm_min_epi32(l, c);
      h = _mm_max_epi32(h, c);
   }

   _mm_storel_epi64((__m128i *)lo, l);
   _mm_storel_epi64((__m128i *)hi, h);
#elif defined(PGS_PAIR_NEON)
   int32x2_t a;
   int32x2_t b;
   int32x2_t c;
   int32x2_t l;
   int32x2_t h;

   a = vld1_s32((const int32_t *)p0);
   b = vld1_s32((const int32_t *)p1);
   l = vmin_s32(a, b);
   h = vmax_s32(a, b);

   if (use_p2)
   {
      c = vld1_s32((const int32_t *)p2);
      l = vmin_s32(l, c);
      h = vmax_s32(h, c);
   }

   vst1_s32((int32_t *)lo, l);
   vst1_s32((int32_t *)hi, h);
#else
   int32_t a[2];
   int32_t b[2];
   int32_t c[2];
   int32_t l[2];
   int32_t h[2];

   memcpy(a, p0, 8);
   memcpy(b, p1, 8);

   l[0] = a[0] < b[0] ? a[0] : b[0];
   l[1] = a[1] < b[1] ? a[1] : b[1];
   h[0] = a[0] > b[0] ? a[0] : b[0];
   h[1] = a[1] > b[1] ? a[1] : b[1];

   if (use_p2)
   {
      memcpy(c, p2, 8);
      l[0] = l[0] < c[0] ? l[0] : c[0];
      l[1] = l[1] < c[1] ? l[1] : c[1];
      h[0] = h[0] > c[0] ? h[0] : c[0];
      h[1] = h[1] > c[1] ? h[1] : c[1];
   }

   memcpy(lo, l, 8);
   memcpy(hi, h, 8);
#endif
}

/* Clamp an int32 pair into [lo_bound, hi_bound], component-wise. */
static PGS_KICK_INLINE void pgs_pair_clamp(
      const void *lo_bound, const void *hi_bound, void *lo, void *hi)
{
#if defined(PGS_PAIR_SSE4)
   __m128i l;
   __m128i h;

   l = _mm_max_epi32(_mm_loadl_epi64((const __m128i *)lo),
                     _mm_loadl_epi64((const __m128i *)lo_bound));
   h = _mm_min_epi32(_mm_loadl_epi64((const __m128i *)hi),
                     _mm_loadl_epi64((const __m128i *)hi_bound));
   _mm_storel_epi64((__m128i *)lo, l);
   _mm_storel_epi64((__m128i *)hi, h);
#else
   int32_t l[2];
   int32_t h[2];
   int32_t lb[2];
   int32_t hb[2];

   memcpy(l, lo, 8); memcpy(h, hi, 8);
   memcpy(lb, lo_bound, 8); memcpy(hb, hi_bound, 8);

   l[0] = l[0] > lb[0] ? l[0] : lb[0];
   l[1] = l[1] > lb[1] ? l[1] : lb[1];
   h[0] = h[0] < hb[0] ? h[0] : hb[0];
   h[1] = h[1] < hb[1] ? h[1] : hb[1];

   memcpy(lo, l, 8); memcpy(hi, h, 8);
#endif
}


/* Component-wise min and max of two int32 pairs. */
static PGS_KICK_INLINE void pgs_pair_min2(const void *a, const void *b, void *out)
{
#if defined(PGS_PAIR_SSE4)
   _mm_storel_epi64((__m128i *)out,
                    _mm_min_epi32(_mm_loadl_epi64((const __m128i *)a),
                                  _mm_loadl_epi64((const __m128i *)b)));
#else
   int32_t x[2];
   int32_t y[2];

   memcpy(x, a, 8);
   memcpy(y, b, 8);
   x[0] = x[0] < y[0] ? x[0] : y[0];
   x[1] = x[1] < y[1] ? x[1] : y[1];
   memcpy(out, x, 8);
#endif
}

static PGS_KICK_INLINE void pgs_pair_max2(const void *a, const void *b, void *out)
{
#if defined(PGS_PAIR_SSE4)
   _mm_storel_epi64((__m128i *)out,
                    _mm_max_epi32(_mm_loadl_epi64((const __m128i *)a),
                                  _mm_loadl_epi64((const __m128i *)b)));
#else
   int32_t x[2];
   int32_t y[2];

   memcpy(x, a, 8);
   memcpy(y, b, 8);
   x[0] = x[0] > y[0] ? x[0] : y[0];
   x[1] = x[1] > y[1] ? x[1] : y[1];
   memcpy(out, x, 8);
#endif
}

#endif
