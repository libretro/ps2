/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef PGS_VERTEX_KERNELS_H
#define PGS_VERTEX_KERNELS_H

/* Vertex-kick kernels for paraLLEl-GS, lifted out of GSInterface so the
 * per-vertex sequences can be pinned by tests and read without the vector
 * template layer around them.
 *
 * The kernels take register words, not muglm types. Every field a kick
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

#endif
