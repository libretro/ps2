#include "GSSpriteSnap.h"

int gs_sprite_snap_edge(int v, int o)
{
   const int e = v - o;
   const int n = -((-e) & ~15);

   if (n + o > 0xffff)
      return ((0xffff - o) & ~15) + o;
   return n + o;
}
