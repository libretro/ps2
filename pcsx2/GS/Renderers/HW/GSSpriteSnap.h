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

#ifdef __cplusplus
}
#endif

#endif
