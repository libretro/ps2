/*
 * Sprite edges at scale (GSSpriteSnap.c, used by the hardware renderer):
 * an edge moves up to the window's next whole pixel, and the coordinate
 * it moves to is one a vertex can carry.
 *
 * A full-screen clear drawn as a sprite from far outside the window, its
 * bottom edge on the last coordinate a vertex holds, must still reach the
 * bottom of the window: an edge moved up past 65535 and cut to 16 bits
 * lands at the top of the coordinate range, and the sprite collapses to a
 * strip above the window, drawing nothing.
 */

#include <stdio.h>

#include "../../pcsx2/GS/Renderers/HW/GSSpriteSnap.c"

/* The snap as the renderer carried it before the edge was bounded: moved
 * up, then stored in the vertex's 16 bits. */
static int snap_unbounded(int v, int o)
{
   const int e = v - o;
   return (-((-e) & ~15) + o) & 0xffff;
}

/* Rows of the window a sprite with vertex coordinates v0 .. v1 (v0 < v1)
 * covers inside rows 0 .. 447, offset o. */
static int rows_covered(int v0, int v1, int o)
{
   const int top = (v0 - o + 15) >> 4;
   const int bot = (v1 - o + 15) >> 4;
   const int a   = top < 0 ? 0 : top;
   const int b   = bot > 448 ? 448 : bot;
   return b > a ? b - a : 0;
}

static int check(int got, int want, const char *what)
{
   if (got != want)
   {
      printf("  %s: %d, expected %d\n", what, got, want);
      return 1;
   }
   return 0;
}

int main(void)
{
   /* The window offset of a 512x448 picture centred at 2048,2048 in
    * the coordinate range, and one a sixteenth off a pixel. */
   const int oy = (2048 - 224) * 16;
   const int ox = (2048 - 256) * 16 + 3;
   int fail = 0;
   int v;

   /* Edges move up to the next whole pixel, in the offset's frame. */
   fail += check(gs_sprite_snap_edge(oy + 16 * 10, oy), oy + 16 * 10, "edge on a pixel");
   fail += check(gs_sprite_snap_edge(oy + 16 * 10 + 1, oy), oy + 16 * 11, "edge past a pixel");
   fail += check(gs_sprite_snap_edge(oy + 16 * 10 + 15, oy), oy + 16 * 11, "edge before a pixel");
   fail += check(gs_sprite_snap_edge(ox + 8, ox), ox + 16, "edge with an odd offset");
   fail += check(gs_sprite_snap_edge(oy - 16 * 5 - 8, oy), oy - 16 * 5, "edge above the window");
   fail += check(gs_sprite_snap_edge(0, 5), 5, "edge at the first coordinate");

   /* Every coordinate a vertex holds snaps to one it holds, on a whole
    * pixel of the window, no lower than it and less than a pixel above
    * it, or on the last whole pixel below the top of the range. */
   for (v = 0; v <= 0xffff; v++)
   {
      const int n = gs_sprite_snap_edge(v, oy);
      const int last = ((0xffff - oy) & ~15) + oy;
      if (n < 0 || n > 0xffff || ((n - oy) & 15) != 0 ||
            (n < v && n != last) || n - v > 15)
      {
         printf("  coordinate %d snaps to %d\n", v, n);
         fail++;
         break;
      }
   }

   /* The clear: a sprite from 1600 rows above the window to the last
    * coordinate, 2271.9375 rows below its origin. */
   {
      const int y0 = oy - 1600 * 16;
      const int y1 = 0xffff;

      fail += check(rows_covered(gs_sprite_snap_edge(y0, oy), gs_sprite_snap_edge(y1, oy), oy),
            448, "rows the snapped clear covers");

      /* Negative control: cut to 16 bits, the bottom edge wraps above the
       * top one, and the clear covers nothing. */
      if (rows_covered(snap_unbounded(y0, oy), snap_unbounded(y1, oy), oy) != 0)
      {
         printf("  negative control: the unbounded snap still covers the window\n");
         fail++;
      }
   }

   printf("sprite edges snap to coordinates a vertex holds: %s\n", fail ? "FAIL" : "ok");
   return fail != 0;
}
