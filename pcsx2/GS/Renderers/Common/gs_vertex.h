/* SPDX-License-Identifier: GPL-3.0+ */
#ifndef GS_VERTEX_H
#define GS_VERTEX_H

/* The GS vertex record and the operations on it, as C89 with a backend per
 * instruction set chosen at run time.
 *
 * The record is 32 bytes and its layout is the one the whole GS agrees on --
 * the GIF register handlers build it, VertexKick stores it, the vertex trace
 * reads it and the HW renderers upload it -- so the layout here is fixed and
 * pinned by tests, not chosen.
 *
 *     bytes  0..7   ST      S, T            two floats
 *     bytes  8..15  RGBAQ   R, G, B, A, Q   four bytes then a float
 *     bytes 16..23  XYZ     X, Y            two u16, then Z as u32
 *     bytes 24..27  UV      U, V            two u16
 *     bytes 28..31  FOG                     u32
 *
 * Backends: SSE2, SSE4.1, AVX and NEON. Nothing here reaches for GSVector,
 * which is SSE4.1-only as a class and so cannot express an SSE2 backend at
 * all; these use the intrinsics directly, which is also what lets the file
 * compile as C89.
 *
 * Dispatch granularity is the point of the design. A field accessor is two
 * or three instructions, so calling one through a pointer would cost several
 * times what it does. So:
 *
 *   - per-vertex operations resolve at compile time, or on a branch over a
 *     cached flag that a loop predicts perfectly;
 *   - batch operations, where the work per call is large enough to absorb it,
 *     go through gs_vertex_ops, resolved once by gs_vertex_init().
 *
 * Shape note: written in the C89 form the emitters in this tree use --
 * declarations at the head of each block, no mixed declarations, no early
 * returns in the middle of a body, block comments.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER)
#define GS_VERTEX_INLINE __forceinline
#elif defined(__GNUC__)
#define GS_VERTEX_INLINE __inline__ __attribute__((always_inline))
#else
#define GS_VERTEX_INLINE
#endif

/* ------------------------------------------------------------------ */
/* Which instruction sets this build can emit at all.                   */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__i386__) || defined(_M_IX86)
#define GS_VERTEX_X86 1
/* immintrin declares every x86 intrinsic regardless of the -m flags in
 * force, which is what lets the wider bodies in gs_vertex.c carry a target
 * attribute instead of needing a translation unit each. The two macros
 * below still say what this build may emit *inline*, which is a separate
 * question from what it may emit behind an attribute. */
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#if defined(__SSE4_1__) || defined(__AVX__) || defined(_MSC_VER)
#define GS_VERTEX_CAN_SSE41 1
#endif
#if defined(__AVX__)
#define GS_VERTEX_CAN_AVX 1
#endif
typedef __m128i gs_vec4i;
typedef __m128  gs_vec4f;
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define GS_VERTEX_NEON 1
#include <arm_neon.h>
typedef int32x4_t   gs_vec4i;
typedef float32x4_t gs_vec4f;
#else
#define GS_VERTEX_SCALAR 1
typedef struct { int32_t v[4]; } gs_vec4i;
typedef struct { float   v[4]; } gs_vec4f;
#endif

/* ------------------------------------------------------------------ */
/* The record.                                                          */
/* ------------------------------------------------------------------ */

struct gs_vertex_st    { float S; float T; };
struct gs_vertex_rgbaq { uint8_t R, G, B, A; float Q; };
struct gs_vertex_xyz   { uint16_t X, Y; uint32_t Z; };

struct gs_vertex_fields
{
   struct gs_vertex_st    ST;
   struct gs_vertex_rgbaq RGBAQ;
   struct gs_vertex_xyz   XYZ;
   uint16_t               U, V;
   uint32_t               FOG;
};

/* 32-byte alignment is load-bearing: it is what lets the whole record move
 * in one instruction where the host has a 32-byte one. */
#if defined(_MSC_VER)
#define GS_VERTEX_ALIGN32 __declspec(align(32))
#elif defined(__GNUC__)
#define GS_VERTEX_ALIGN32 __attribute__((aligned(32)))
#else
#define GS_VERTEX_ALIGN32
#endif

union GS_VERTEX_ALIGN32 gs_vertex
{
   struct gs_vertex_fields f;
   gs_vec4i               m[2];
   uint32_t               w[8];
   uint8_t                b[32];
};

/* ------------------------------------------------------------------ */
/* Run-time backend selection.                                          */
/* ------------------------------------------------------------------ */

enum
{
   GS_VERTEX_BACKEND_SCALAR = 0,
   GS_VERTEX_BACKEND_SSE2   = 1,
   GS_VERTEX_BACKEND_SSE41  = 2,
   GS_VERTEX_BACKEND_AVX    = 3,
   GS_VERTEX_BACKEND_NEON   = 4
};

/* Batch operations. One indirect call covers n vertices, so the call costs
 * nothing per vertex. */
struct gs_vertex_ops
{
   void (*copy)(union gs_vertex *dst, const union gs_vertex *src, size_t n);
   void (*gather_xy)(int32_t *dst, const union gs_vertex *src, size_t n);
   void (*gather_uv)(int32_t *dst, const union gs_vertex *src, size_t n);
   void (*minmax_xy)(int32_t *lo, int32_t *hi, const union gs_vertex *src, size_t n);
   const char *name;
   int backend;
};

extern const struct gs_vertex_ops *gs_vertex_op;

/* True when the host has a 32-byte move. It reports what gs_vertex_op will
 * do in bulk; it cannot change what gs_vertex_store emits, because that is
 * inlined into the caller and so fixed by the flags that translation unit
 * was built with. Going out of line to reach a wider store would cost more
 * than the store saves -- the batch copy is the runtime-dispatched path. */
extern int gs_vertex_wide_store;

/* Resolves both of the above from libretro-common's cpu_features_get().
 * Idempotent; call before any other entry point here. */
void gs_vertex_init(void);

/* Pins one backend, for triage and for tests that need to reach a body the
 * host would otherwise skip over. Refuses, returning -1 and changing
 * nothing, for a backend this CPU cannot execute or this build does not
 * contain; returns 0 on success. Scalar is always available. */
int gs_vertex_set_backend(int backend);

/* ------------------------------------------------------------------ */
/* Per-vertex store.                                                    */
/*                                                                      */
/* The hot one: VertexKick runs this for every vertex the GIF delivers.  */
/* One 32-byte move where the build has AVX, two 16-byte ones otherwise. */
/* Compile-time, deliberately: a call to reach a wider store costs more  */
/* than the store saves at this size.                                    */
/* ------------------------------------------------------------------ */

static GS_VERTEX_INLINE void gs_vertex_store(
      union gs_vertex *dst, const union gs_vertex *src)
{
#if defined(GS_VERTEX_CAN_AVX)
   _mm256_store_si256((__m256i *)dst, _mm256_load_si256((const __m256i *)src));
#elif defined(GS_VERTEX_X86)
   _mm_store_si128((__m128i *)dst,       _mm_load_si128((const __m128i *)src));
   _mm_store_si128(((__m128i *)dst) + 1, _mm_load_si128(((const __m128i *)src) + 1));
#elif defined(GS_VERTEX_NEON)
   vst1q_s32((int32_t *)dst,       vld1q_s32((const int32_t *)src));
   vst1q_s32(((int32_t *)dst) + 4, vld1q_s32(((const int32_t *)src) + 4));
#else
   memcpy(dst, src, 32);
#endif
}

/* ------------------------------------------------------------------ */
/* Field accessors.                                                     */
/*                                                                      */
/* Each yields the same four lanes the GS code expects, and each is two  */
/* or three instructions -- which is exactly why none of them dispatches */
/* through a pointer. Where SSE4.1 has a shorter encoding for one, it is */
/* taken; the SSE2 body is the contract.                                 */
/* ------------------------------------------------------------------ */

/* { X, Y, X, Y }, zero-extended from u16. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_xy(const union gs_vertex *v)
{
#if defined(GS_VERTEX_CAN_SSE41)
   /* pmovzxwd takes the low four u16 straight to u32.
    *
    * Reading the whole of m[1], not just the 8 bytes that hold X,Y: a
    * narrow load measures better in isolation but worse where it is
    * actually called. GSState fetches XY, UV, Z and FOG from one vertex in
    * one expression, so m[1] wants to be loaded once and shuffled four
    * ways; a per-accessor narrow load cannot be shared and costs more than
    * the folding saves. */
   return _mm_shuffle_epi32(_mm_cvtepu16_epi32(v->m[1]), 0x44);
#elif defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(_mm_unpacklo_epi16(v->m[1], _mm_setzero_si128()), 0x44);
#elif defined(GS_VERTEX_NEON)
   {
      uint32x4_t w = vmovl_u16(vget_low_u16(vreinterpretq_u16_s32(v->m[1])));
      int32x2_t  l = vget_low_s32(vreinterpretq_s32_u32(w));
      return vcombine_s32(l, l);
   }
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.XYZ.X; r.v[1] = (int32_t)v->f.XYZ.Y;
      r.v[2] = r.v[0];              r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { U, V, U, V }, zero-extended from u16. No pmovzx reaches the high half,
 * so every backend unpacks here. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_uv(const union gs_vertex *v)
{
   /* Shares the m[1] load with gs_vertex_xy/z/fog -- see the note there. */
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(_mm_unpackhi_epi16(v->m[1], _mm_setzero_si128()), 0x44);
#elif defined(GS_VERTEX_NEON)
   {
      uint32x4_t w = vmovl_u16(vget_high_u16(vreinterpretq_u16_s32(v->m[1])));
      int32x2_t  l = vget_low_s32(vreinterpretq_s32_u32(w));
      return vcombine_s32(l, l);
   }
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.U; r.v[1] = (int32_t)v->f.V;
      r.v[2] = r.v[0];          r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { Z, Z, Z, Z } */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_z(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(v->m[1], 0x55);
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_s32(v->m[1], 1);
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.XYZ.Z; r.v[1] = r.v[0];
      r.v[2] = r.v[0];              r.v[3] = r.v[0];
      return r;
   }
#endif
}

/* { FOG, FOG, FOG, FOG } */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_fog(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   return _mm_shuffle_epi32(v->m[1], 0xff);
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_s32(v->m[1], 3);
#else
   {
      gs_vec4i r;
      r.v[0] = (int32_t)v->f.FOG; r.v[1] = r.v[0];
      r.v[2] = r.v[0];            r.v[3] = r.v[0];
      return r;
   }
#endif
}

/* { R, G, B, A } as four int32. */
static GS_VERTEX_INLINE gs_vec4i gs_vertex_rgba(const union gs_vertex *v)
{
#if defined(GS_VERTEX_CAN_SSE41)
   /* The four bytes sit at offset 8; one pmovzxbd covers the whole widen. */
   return _mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)v->w[2]));
#elif defined(GS_VERTEX_X86)
   {
      __m128i z = _mm_setzero_si128();
      return _mm_unpacklo_epi16(_mm_unpackhi_epi8(v->m[0], z), z);
   }
#elif defined(GS_VERTEX_NEON)
   {
      uint8x8_t  b  = vreinterpret_u8_u32(vdup_n_u32(v->w[2]));
      uint16x8_t w  = vmovl_u8(b);
      return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(w)));
   }
#else
   {
      gs_vec4i r;
      r.v[0] = v->f.RGBAQ.R; r.v[1] = v->f.RGBAQ.G;
      r.v[2] = v->f.RGBAQ.B; r.v[3] = v->f.RGBAQ.A;
      return r;
   }
#endif
}

/* { S, T, S, T } */
static GS_VERTEX_INLINE gs_vec4f gs_vertex_st(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   /* Both spellings below are the same instruction -- pshufd $0x44, which
    * takes its operand straight from memory -- but each compiler keeps one
    * and canonicalises the other into movaps + movlhps, which is a whole
    * instruction more because movlhps cannot fold the load. So pick the
    * spelling each one leaves alone. */
#if defined(__clang__)
   return _mm_castsi128_ps(_mm_unpacklo_epi64(v->m[0], v->m[0]));
#else
   return _mm_castsi128_ps(_mm_shuffle_epi32(v->m[0], 0x44));
#endif
#elif defined(GS_VERTEX_NEON)
   {
      float32x4_t f = vreinterpretq_f32_s32(v->m[0]);
      float32x2_t l = vget_low_f32(f);
      return vcombine_f32(l, l);
   }
#else
   {
      gs_vec4f r;
      r.v[0] = v->f.ST.S; r.v[1] = v->f.ST.T;
      r.v[2] = r.v[0];    r.v[3] = r.v[1];
      return r;
   }
#endif
}

/* { Q, Q, Q, Q } */
static GS_VERTEX_INLINE gs_vec4f gs_vertex_q(const union gs_vertex *v)
{
#if defined(GS_VERTEX_X86)
   /* Same reason as gs_vertex_st: pshufd folds the load, shufps does not. */
   return _mm_castsi128_ps(_mm_shuffle_epi32(v->m[0], 0xff));
#elif defined(GS_VERTEX_NEON)
   return vdupq_laneq_f32(vreinterpretq_f32_s32(v->m[0]), 3);
#else
   {
      gs_vec4f r;
      r.v[0] = v->f.RGBAQ.Q; r.v[1] = r.v[0];
      r.v[2] = r.v[0];       r.v[3] = r.v[0];
      return r;
   }
#endif
}

#endif
