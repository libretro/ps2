/* SPDX-License-Identifier: GPL-3.0+ */

/* Batch operations on the GS vertex record, one body per instruction set,
 * selected once at start-up from libretro-common's CPU probe.
 *
 * Only batch work dispatches. A field accessor is two or three instructions
 * and lives inline in the header; routing one of those through a pointer
 * would cost several times what the accessor does. These take a count, so
 * the single indirect call is amortised over the whole run and the body can
 * use whatever the host actually has.
 *
 * The AVX bodies carry a target attribute rather than living in their own
 * translation unit, so this file still compiles once, at the baseline, and
 * the wide code is reachable on a host that has it.
 */

#include "gs_vertex.h"

#if !defined(GS_VERTEX_NO_LIBRETRO)
#include <features/features_cpu.h>
#include <libretro.h>
#endif

#if defined(GS_VERTEX_X86) && (defined(__GNUC__) || defined(__clang__)) && \
    !defined(_MSC_VER) && !defined(GS_VERTEX_NO_MULTIVERSION)
/* GNU-mode GCC and clang attach target attributes to the intrinsic
 * declarations themselves, so every one is available whatever the baseline
 * is and a body can carry its own target. That is what lets one
 * translation unit hold every tier. */
#define GS_VERTEX_TARGET(x) __attribute__((target(x)))
#define GS_VERTEX_MULTIVERSION 1
#else
/* Everywhere else a body may only use what the baseline already declares.
 * cl.exe declares every intrinsic and has no target attribute; clang-cl
 * gates the declarations on /arch. Compiling a tier above the baseline is
 * therefore not portable, so the tiers below fall back to the widest one
 * this build can name, and dispatch caps at the same place.
 *
 * Define GS_VERTEX_NO_MULTIVERSION to take this path deliberately, which
 * is how the harness reaches it on a compiler that has the attributes. */
#define GS_VERTEX_TARGET(x)
#endif

#if defined(GS_VERTEX_MULTIVERSION) || defined(GS_VERTEX_CAN_SSE41)
#define GS_VERTEX_BUILD_SSE41 1
#endif
#if defined(GS_VERTEX_MULTIVERSION) || defined(GS_VERTEX_CAN_AVX)
#define GS_VERTEX_BUILD_AVX 1
#endif

int gs_vertex_wide_store = 0;

/* ------------------------------------------------------------------ */
/* Scalar -- the contract every vector body below is checked against.   */
/* ------------------------------------------------------------------ */

static void gs_copy_scalar(union gs_vertex *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i < n; i++)
      memcpy(&dst[i], &src[i], 32);
}

static void gs_gather_xy_scalar(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.XYZ.X;
      dst[2 * i + 1] = (int32_t)src[i].f.XYZ.Y;
   }
}

static void gs_gather_uv_scalar(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.U;
      dst[2 * i + 1] = (int32_t)src[i].f.V;
   }
}

static void gs_minmax_xy_scalar(int32_t *lo, int32_t *hi, const union gs_vertex *src, size_t n)
{
   int32_t lx = 0x7fffffff, ly = 0x7fffffff;
   int32_t hx = -0x7fffffff - 1, hy = -0x7fffffff - 1;
   size_t i;

   for (i = 0; i < n; i++)
   {
      int32_t x = (int32_t)src[i].f.XYZ.X;
      int32_t y = (int32_t)src[i].f.XYZ.Y;

      if (x < lx) lx = x;
      if (y < ly) ly = y;
      if (x > hx) hx = x;
      if (y > hy) hy = y;
   }

   lo[0] = lx; lo[1] = ly;
   hi[0] = hx; hi[1] = hy;
}

static const struct gs_vertex_ops gs_ops_scalar =
{
   gs_copy_scalar, gs_gather_xy_scalar, gs_gather_uv_scalar, gs_minmax_xy_scalar,
   "scalar", GS_VERTEX_BACKEND_SCALAR
};

const struct gs_vertex_ops *gs_vertex_op = &gs_ops_scalar;

#if defined(GS_VERTEX_X86)

/* ------------------------------------------------------------------ */
/* SSE2.                                                                */
/* ------------------------------------------------------------------ */

/* Two records per iteration: the loads and stores pair, and the loop
 * carries no dependency, so this runs at store throughput. */
static void gs_copy_sse2(union gs_vertex *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i a = _mm_load_si128(((const __m128i *)&src[i]) + 0);
      __m128i b = _mm_load_si128(((const __m128i *)&src[i]) + 1);
      __m128i c = _mm_load_si128(((const __m128i *)&src[i]) + 2);
      __m128i d = _mm_load_si128(((const __m128i *)&src[i]) + 3);

      _mm_store_si128(((__m128i *)&dst[i]) + 0, a);
      _mm_store_si128(((__m128i *)&dst[i]) + 1, b);
      _mm_store_si128(((__m128i *)&dst[i]) + 2, c);
      _mm_store_si128(((__m128i *)&dst[i]) + 3, d);
   }

   for (; i < n; i++)
   {
      _mm_store_si128(((__m128i *)&dst[i]) + 0, _mm_load_si128(((const __m128i *)&src[i]) + 0));
      _mm_store_si128(((__m128i *)&dst[i]) + 1, _mm_load_si128(((const __m128i *)&src[i]) + 1));
   }
}

/* XY is one dword at offset 16 of each record, so two records supply one
 * packed pair each; unpack them together and the widen is one instruction
 * for both. */
static void gs_gather_xy_sse2(int32_t *dst, const union gs_vertex *src, size_t n)
{
   __m128i z = _mm_setzero_si128();
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[4]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[4]));
      _mm_storeu_si128((__m128i *)&dst[2 * i], _mm_unpacklo_epi16(p, z));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.XYZ.X;
      dst[2 * i + 1] = (int32_t)src[i].f.XYZ.Y;
   }
}

static void gs_gather_uv_sse2(int32_t *dst, const union gs_vertex *src, size_t n)
{
   __m128i z = _mm_setzero_si128();
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[6]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[6]));
      _mm_storeu_si128((__m128i *)&dst[2 * i], _mm_unpacklo_epi16(p, z));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.U;
      dst[2 * i + 1] = (int32_t)src[i].f.V;
   }
}

/* X and Y are u16 widened to int32, so they are non-negative and a signed
 * compare orders them correctly. SSE2 has no 32-bit min or max, so the
 * select is compare-and-blend. */
static void gs_minmax_xy_sse2(int32_t *lo, int32_t *hi, const union gs_vertex *src, size_t n)
{
   __m128i z  = _mm_setzero_si128();
   __m128i l  = _mm_set1_epi32(0x7fffffff);
   __m128i h  = _mm_set1_epi32(-0x7fffffff - 1);
   int32_t lt[4];
   int32_t ht[4];
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[4]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[4]));
      __m128i v = _mm_unpacklo_epi16(p, z);
      __m128i gl = _mm_cmpgt_epi32(l, v);
      __m128i gh = _mm_cmpgt_epi32(v, h);

      l = _mm_or_si128(_mm_and_si128(gl, v), _mm_andnot_si128(gl, l));
      h = _mm_or_si128(_mm_and_si128(gh, v), _mm_andnot_si128(gh, h));
   }

   _mm_storeu_si128((__m128i *)lt, l);
   _mm_storeu_si128((__m128i *)ht, h);

   lo[0] = lt[0] < lt[2] ? lt[0] : lt[2];
   lo[1] = lt[1] < lt[3] ? lt[1] : lt[3];
   hi[0] = ht[0] > ht[2] ? ht[0] : ht[2];
   hi[1] = ht[1] > ht[3] ? ht[1] : ht[3];

   for (; i < n; i++)
   {
      int32_t x = (int32_t)src[i].f.XYZ.X;
      int32_t y = (int32_t)src[i].f.XYZ.Y;

      if (x < lo[0]) lo[0] = x;
      if (y < lo[1]) lo[1] = y;
      if (x > hi[0]) hi[0] = x;
      if (y > hi[1]) hi[1] = y;
   }
}

static const struct gs_vertex_ops gs_ops_sse2 =
{
   gs_copy_sse2, gs_gather_xy_sse2, gs_gather_uv_sse2, gs_minmax_xy_sse2,
   "sse2", GS_VERTEX_BACKEND_SSE2
};

#if defined(GS_VERTEX_BUILD_SSE41)

/* ------------------------------------------------------------------ */
/* SSE4.1 -- pmovzx replaces the unpack, pminsd/pmaxsd the blend.       */
/* ------------------------------------------------------------------ */

GS_VERTEX_TARGET("sse4.1")
static void gs_gather_xy_sse41(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[4]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[4]));
      _mm_storeu_si128((__m128i *)&dst[2 * i], _mm_cvtepu16_epi32(p));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.XYZ.X;
      dst[2 * i + 1] = (int32_t)src[i].f.XYZ.Y;
   }
}

GS_VERTEX_TARGET("sse4.1")
static void gs_gather_uv_sse41(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[6]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[6]));
      _mm_storeu_si128((__m128i *)&dst[2 * i], _mm_cvtepu16_epi32(p));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.U;
      dst[2 * i + 1] = (int32_t)src[i].f.V;
   }
}

GS_VERTEX_TARGET("sse4.1")
static void gs_minmax_xy_sse41(int32_t *lo, int32_t *hi, const union gs_vertex *src, size_t n)
{
   __m128i l = _mm_set1_epi32(0x7fffffff);
   __m128i h = _mm_set1_epi32(-0x7fffffff - 1);
   int32_t lt[4];
   int32_t ht[4];
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m128i p = _mm_unpacklo_epi32(_mm_cvtsi32_si128((int)src[i + 0].w[4]),
                                     _mm_cvtsi32_si128((int)src[i + 1].w[4]));
      __m128i v = _mm_cvtepu16_epi32(p);

      l = _mm_min_epi32(l, v);
      h = _mm_max_epi32(h, v);
   }

   _mm_storeu_si128((__m128i *)lt, l);
   _mm_storeu_si128((__m128i *)ht, h);

   lo[0] = lt[0] < lt[2] ? lt[0] : lt[2];
   lo[1] = lt[1] < lt[3] ? lt[1] : lt[3];
   hi[0] = ht[0] > ht[2] ? ht[0] : ht[2];
   hi[1] = ht[1] > ht[3] ? ht[1] : ht[3];

   for (; i < n; i++)
   {
      int32_t x = (int32_t)src[i].f.XYZ.X;
      int32_t y = (int32_t)src[i].f.XYZ.Y;

      if (x < lo[0]) lo[0] = x;
      if (y < lo[1]) lo[1] = y;
      if (x > hi[0]) hi[0] = x;
      if (y > hi[1]) hi[1] = y;
   }
}

static const struct gs_vertex_ops gs_ops_sse41 =
{
   gs_copy_sse2, gs_gather_xy_sse41, gs_gather_uv_sse41, gs_minmax_xy_sse41,
   "sse4.1", GS_VERTEX_BACKEND_SSE41
};

#endif /* GS_VERTEX_BUILD_SSE41 */

#if defined(GS_VERTEX_BUILD_AVX)

/* ------------------------------------------------------------------ */
/* AVX -- the whole record moves in one instruction.                    */
/* ------------------------------------------------------------------ */

GS_VERTEX_TARGET("avx")
static void gs_copy_avx(union gs_vertex *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      __m256i a = _mm256_load_si256(((const __m256i *)src) + i + 0);
      __m256i b = _mm256_load_si256(((const __m256i *)src) + i + 1);

      _mm256_store_si256(((__m256i *)dst) + i + 0, a);
      _mm256_store_si256(((__m256i *)dst) + i + 1, b);
   }

   for (; i < n; i++)
      _mm256_store_si256(((__m256i *)dst) + i,
                         _mm256_load_si256(((const __m256i *)src) + i));
}

static const struct gs_vertex_ops gs_ops_avx =
{
   gs_copy_avx, gs_gather_xy_sse41, gs_gather_uv_sse41, gs_minmax_xy_sse41,
   "avx", GS_VERTEX_BACKEND_AVX
};

#endif /* GS_VERTEX_BUILD_AVX */

#endif /* GS_VERTEX_X86 */

#if defined(GS_VERTEX_NEON)

static void gs_copy_neon(union gs_vertex *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i < n; i++)
   {
      int32x4_t a = vld1q_s32(((const int32_t *)&src[i]) + 0);
      int32x4_t b = vld1q_s32(((const int32_t *)&src[i]) + 4);

      vst1q_s32(((int32_t *)&dst[i]) + 0, a);
      vst1q_s32(((int32_t *)&dst[i]) + 4, b);
   }
}

static void gs_gather_xy_neon(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      uint32x2_t p = vset_lane_u32(src[i + 1].w[4], vdup_n_u32(src[i + 0].w[4]), 1);
      uint32x4_t w = vmovl_u16(vreinterpret_u16_u32(p));

      vst1q_s32(&dst[2 * i], vreinterpretq_s32_u32(w));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.XYZ.X;
      dst[2 * i + 1] = (int32_t)src[i].f.XYZ.Y;
   }
}

static void gs_gather_uv_neon(int32_t *dst, const union gs_vertex *src, size_t n)
{
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      uint32x2_t p = vset_lane_u32(src[i + 1].w[6], vdup_n_u32(src[i + 0].w[6]), 1);
      uint32x4_t w = vmovl_u16(vreinterpret_u16_u32(p));

      vst1q_s32(&dst[2 * i], vreinterpretq_s32_u32(w));
   }

   for (; i < n; i++)
   {
      dst[2 * i + 0] = (int32_t)src[i].f.U;
      dst[2 * i + 1] = (int32_t)src[i].f.V;
   }
}

static void gs_minmax_xy_neon(int32_t *lo, int32_t *hi, const union gs_vertex *src, size_t n)
{
   int32x4_t l = vdupq_n_s32(0x7fffffff);
   int32x4_t h = vdupq_n_s32(-0x7fffffff - 1);
   int32_t lt[4];
   int32_t ht[4];
   size_t i;

   for (i = 0; i + 2 <= n; i += 2)
   {
      uint32x2_t p = vset_lane_u32(src[i + 1].w[4], vdup_n_u32(src[i + 0].w[4]), 1);
      int32x4_t  v = vreinterpretq_s32_u32(vmovl_u16(vreinterpret_u16_u32(p)));

      l = vminq_s32(l, v);
      h = vmaxq_s32(h, v);
   }

   vst1q_s32(lt, l);
   vst1q_s32(ht, h);

   lo[0] = lt[0] < lt[2] ? lt[0] : lt[2];
   lo[1] = lt[1] < lt[3] ? lt[1] : lt[3];
   hi[0] = ht[0] > ht[2] ? ht[0] : ht[2];
   hi[1] = ht[1] > ht[3] ? ht[1] : ht[3];

   for (; i < n; i++)
   {
      int32_t x = (int32_t)src[i].f.XYZ.X;
      int32_t y = (int32_t)src[i].f.XYZ.Y;

      if (x < lo[0]) lo[0] = x;
      if (y < lo[1]) lo[1] = y;
      if (x > hi[0]) hi[0] = x;
      if (y > hi[1]) hi[1] = y;
   }
}

static const struct gs_vertex_ops gs_ops_neon =
{
   gs_copy_neon, gs_gather_xy_neon, gs_gather_uv_neon, gs_minmax_xy_neon,
   "neon", GS_VERTEX_BACKEND_NEON
};

#endif /* GS_VERTEX_NEON */

/* ------------------------------------------------------------------ */

/* The widest tier this build actually contains. Dispatch caps here, so a
 * build that could not name a tier never selects it. */
static int gs_vertex_built_backend(void)
{
#if defined(GS_VERTEX_NEON)
   return GS_VERTEX_BACKEND_NEON;
#elif defined(GS_VERTEX_X86)
#if defined(GS_VERTEX_BUILD_AVX)
   return GS_VERTEX_BACKEND_AVX;
#elif defined(GS_VERTEX_BUILD_SSE41)
   return GS_VERTEX_BACKEND_SSE41;
#else
   return GS_VERTEX_BACKEND_SSE2;
#endif
#else
   return GS_VERTEX_BACKEND_SCALAR;
#endif
}

/* Best backend this host can actually execute, never wider than what this
 * build contains. Everything below selects against this, so no path can
 * hand the CPU an instruction it lacks or call a body that is not here. */
static int gs_vertex_host_backend(void)
{
   int built = gs_vertex_built_backend();
   int want;

#if defined(GS_VERTEX_NEON)
   want = GS_VERTEX_BACKEND_NEON;
#elif defined(GS_VERTEX_X86) && !defined(GS_VERTEX_NO_LIBRETRO)
   {
      uint64_t cpu = cpu_features_get();

      if ((cpu & RETRO_SIMD_AVX) != 0)
         want = GS_VERTEX_BACKEND_AVX;
      else if ((cpu & RETRO_SIMD_SSE4) != 0)
         want = GS_VERTEX_BACKEND_SSE41;
      else
         want = GS_VERTEX_BACKEND_SSE2;
   }
#elif defined(GS_VERTEX_X86)
   /* No CPU probe available, so take what this build was told it has. */
#if defined(GS_VERTEX_CAN_AVX)
   want = GS_VERTEX_BACKEND_AVX;
#elif defined(GS_VERTEX_CAN_SSE41)
   want = GS_VERTEX_BACKEND_SSE41;
#else
   want = GS_VERTEX_BACKEND_SSE2;
#endif
#else
   want = GS_VERTEX_BACKEND_SCALAR;
#endif

   return want < built ? want : built;
}

/* Installs a backend without asking whether the host can run it. Private,
 * and only ever reached through the two entry points below, which check. */
static void gs_vertex_install(int backend)
{
   switch (backend)
   {
#if defined(GS_VERTEX_X86)
      case GS_VERTEX_BACKEND_SSE2:
         gs_vertex_op = &gs_ops_sse2;
         gs_vertex_wide_store = 0;
         break;
#if defined(GS_VERTEX_BUILD_SSE41)
      case GS_VERTEX_BACKEND_SSE41:
         gs_vertex_op = &gs_ops_sse41;
         gs_vertex_wide_store = 0;
         break;
#endif
#if defined(GS_VERTEX_BUILD_AVX)
      case GS_VERTEX_BACKEND_AVX:
         gs_vertex_op = &gs_ops_avx;
         gs_vertex_wide_store = 1;
         break;
#endif
#endif
#if defined(GS_VERTEX_NEON)
      case GS_VERTEX_BACKEND_NEON:
         gs_vertex_op = &gs_ops_neon;
         gs_vertex_wide_store = 0;
         break;
#endif
      default:
         gs_vertex_op = &gs_ops_scalar;
         gs_vertex_wide_store = 0;
         break;
   }
}

void gs_vertex_init(void)
{
   gs_vertex_install(gs_vertex_host_backend());
}

int gs_vertex_set_backend(int backend)
{
   /* Scalar is always available. Anything else has to be one this host can
    * execute: the ladder is ordered, so "not above the host's own" is the
    * whole test. NEON and the x86 rungs never coexist in one build, so a
    * cross-ISA request fails the same comparison. */
   if (backend != GS_VERTEX_BACKEND_SCALAR)
   {
      int host = gs_vertex_host_backend();

      if (backend < GS_VERTEX_BACKEND_SCALAR || backend > host)
         return -1;
#if defined(GS_VERTEX_NEON)
      if (backend != GS_VERTEX_BACKEND_NEON)
         return -1;
#elif !defined(GS_VERTEX_X86)
      return -1;
#endif
   }

   gs_vertex_install(backend);
   return 0;
}
