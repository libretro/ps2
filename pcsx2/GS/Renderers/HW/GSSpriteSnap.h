/*
 * Sprite edges at scale, for the hardware renderer.
 *
 * A sprite covers the native pixels whose positions its edges enclose:
 * rows ceil(y0) to ceil(y1) - 1 of the window, and the same across. Drawn
 * scaled, it covers from its edges scaled instead, so each edge moves to
 * the native pixel position the GS covers from.
 */

#ifndef GS_SPRITE_SNAP_H
#define GS_SPRITE_SNAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* The vertex coordinate an edge moves to: `v` is the coordinate the
 * vertex carries (12.4 fixed point, 0 .. 65535) and `o` the context's
 * offset in the same units, so the edge sits at (v - o) / 16 in the
 * window. It moves up to the next whole pixel of the window. An edge
 * whose pixel lies past the last coordinate a vertex can carry stays on
 * the last whole pixel it can: the window ends at 2047, so the pixels the
 * edge gives up are never drawn. */
int gs_sprite_snap_edge(int v, int o);

/* A sprite sampled nearest: natively each pixel reads the texel under
 * its position, the pixel's top left corner. Drawn scaled, the fragments
 * of a pixel sit across it, and a coordinate that puts a texel boundary
 * inside the pixel has part of them read the next texel: a copy of a
 * picture onto itself one pixel along, which the GS makes exactly, comes
 * out half a pixel along, and the edge of a strip copied that way reads
 * past the strip. When the sprite steps a whole number of texels a pixel,
 * the texel boundaries are moved onto the pixel boundaries - the fragments
 * of a pixel then read the texels from the one its position reads, across
 * the texels it steps over, as a scaled texture's own do - and this gives
 * the amount, in texels, to add to the coordinates along the axis. u_lo
 * and u_hi are the coordinates, in texels, at the sprite's two edges along
 * the axis, p_lo < p_hi, which sit on whole pixels (1/16 pixel units).
 * Any other step, and a sprite covering no pixel, gives 0. */
float gs_sprite_fit_nearest(float u_lo, float u_hi, int p_lo, int p_hi);

#ifdef __cplusplus
}
#endif

#endif
