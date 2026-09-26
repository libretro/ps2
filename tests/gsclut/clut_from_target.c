/* Palettes the GPU drew, read from the target that holds them.
 *
 * A game can draw its palettes: render colours into a page, then load that
 * page as a CLUT. Local memory never sees those colours, so GSClut::Read32
 * takes the palette from the render target holding its blocks
 * (GSTextureCache::LookupPaletteSource), through ps_convert_clut_8/_4.
 *
 * Pinned here, each against the form it replaces as a negative control:
 *
 *  1. At an integer scale the copy keeps the target's samples: an entry is
 *     `samples` texels wide in a palette `samples` rows tall, and sample_p
 *     (tfx) reads the sample of the fragment's place in its native pixel,
 *     under either native pixel grid (NativeOffset 0 or half a pixel). So a
 *     palette built by drawing at scale gives every fragment the colour its
 *     own sample was graded to. The one-row copy hands every fragment the
 *     entry's first sample.
 *
 *  2. Which target serves the palette: one starting at CBP before one
 *     containing it; a containing one only when laid out as the palette is;
 *     and only where it holds the area (valid rect), the EE has not written
 *     it since (dirty rects), and it has drawn some of it since local memory
 *     last took its data (drawn_since_read) -- elsewhere local memory holds
 *     the palette exactly and the target an upscaled, possibly filtered,
 *     copy. The single-pass lookup takes the first target in the list and
 *     neither of the last two tests.
 *
 *  3. The palette is the one the last load put in the CLUT buffer. A change
 *     of CBP alone is outside TEX0's dirty mask (GSState::ApplyTEX0), so a
 *     draw flushed from the saved register state carries an older CBP; the
 *     load's CBP names the palette. Modelled on GSState's snapshot rules.
 *
 * Build and run, from tests/gsclut:
 *   cc -O2 -std=c89 -pedantic -Wall clut_from_target.c -o clut_from_target
 *   ./clut_from_target
 */
#include <math.h>
#include <stdio.h>

typedef unsigned int u32;

/* ---- 1. the sampled palette ------------------------------------------------ */

/* ps_convert_clut_8's entry position: 8 groups of 16x2, the top-right and
 * bottom-left quadrants swapped, the block clamped at 15. */
static void clut8_pos(u32 clut_x, u32 doffset, u32* px, u32* py)
{
   u32 block = (clut_x >> 4) + (doffset >> 4);
   u32 index, subgroup;
   if (block > 15u)
      block = 15u;
   index = (block << 4) | (clut_x & 15u);
   subgroup = (index / 8u) % 4u;
   *px = (index % 8u) + ((subgroup >= 2u) ? 8u : 0u);
   *py = ((index / 32u) * 2u) + (subgroup % 2u);
}

/* The target, at scale S: a sample's value names its place. */
static u32 target_sample(u32 x, u32 y)
{
   return (y << 16) | x;
}

/* ps_convert_clut_8: the palette texel (tx, ty) of a copy with `samples`
 * samples (1 for the one-row copy), from the target at `scale`. */
static u32 copy_texel(u32 tx, u32 ty, u32 samples, float scale, u32 ox, u32 oy, u32 doffset)
{
   const u32 n = samples ? samples : 1u;
   u32 px, py;
   clut8_pos(tx / n, doffset, &px, &py);
   if (n > 1u)
      return target_sample((ox + px) * n + tx % n, (oy + py) * n + ty);
   return target_sample((u32)floor((ox + px) * scale), (u32)floor((oy + py) * scale));
}

/* tfx sample_p: the palette texel an index reads from fragment (fx, fy) of a
 * draw at scale s with its native grid NativeOffset in. */
static u32 sample_p(u32 idx, double fx, double fy, double s, double native_offset, u32 rows, float scale,
   u32 ox, u32 oy, u32 doffset)
{
   int subx = 0, suby = 0;
   const int n = (int)rows;
   if (n > 1)
   {
      const double sx = floor((fx - native_offset) / s) * s + native_offset;
      const double sy = floor((fy - native_offset) / s) * s + native_offset;
      subx = (int)((fx - sx) * n / s);
      suby = (int)((fy - sy) * n / s);
      if (subx > n - 1)
         subx = n - 1;
      if (suby > n - 1)
         suby = n - 1;
   }
   return copy_texel(idx * (u32)n + (u32)subx, (u32)suby, rows, scale, ox, oy, doffset);
}

/* Over every entry, every fragment of a native pixel, both grids: does the
 * fragment read its own sample of the entry? */
static int sampled_palette_holds(int sampled)
{
   static const u32 scales[3] = { 2, 3, 4 };
   static const double grids[2] = { 0.0, 0.5 };
   const u32 ox = 16, oy = 16, doffset = 0x20;
   u32 si, gi, e, i, j;
   for (si = 0; si < 3; si++)
      for (gi = 0; gi < 2; gi++)
      {
         const u32 S = scales[si];
         const double N = grids[gi] * S;
         for (e = 0; e < 256; e++)
            for (j = 0; j < S; j++)
               for (i = 0; i < S; i++)
               {
                  /* native pixel 5,7 of the draw; fragment i,j inside it */
                  const double fx = 5.0 * S + N + i + 0.5, fy = 7.0 * S + N + j + 0.5;
                  u32 px, py, want, got;
                  clut8_pos(e, doffset, &px, &py);
                  want = target_sample((ox + px) * S + i, (oy + py) * S + j);
                  got = sample_p(e, fx, fy, (double)S, N, sampled ? S : 1u, (float)S, ox, oy, doffset);
                  if (got != want)
                     return 0;
               }
      }
   return 1;
}

static int check_sampled_palette(void)
{
   int fail = 0;
   /* At native, one row, the copy is the old one. */
   {
      u32 e, px, py;
      for (e = 0; e < 256; e++)
      {
         clut8_pos(e, 0, &px, &py);
         if (sample_p(e, 3.5, 9.5, 1.0, 0.0, 1, 1.0f, 0, 0, 0) != target_sample(px, py))
         {
            printf("  native entry %u moved\n", e);
            fail++;
            break;
         }
      }
   }
   if (!sampled_palette_holds(1))
   {
      printf("  a fragment does not read its own sample of the entry\n");
      fail++;
   }
   if (sampled_palette_holds(0))
   {
      printf("  negative: the one-row copy keeps the samples\n");
      fail++;
   }
   return fail;
}

/* ---- 2. which target serves the palette ---------------------------------- */

struct rect
{
   int x0, y0, x1, y1; /* x1, y1 exclusive; empty when x0 >= x1 */
};

struct target
{
   u32 tbp, end, psm, swizzle; /* swizzle: the layout class HasSameSwizzleBits compares */
   int contains_ok;            /* ComputeSurfaceOffset found the palette */
   int ox, oy;                 /* where the palette sits in it */
   struct rect valid, drawn, dirty;
};

static int rect_empty(struct rect r)
{
   return r.x0 >= r.x1 || r.y0 >= r.y1;
}

static struct rect rect_and(struct rect a, struct rect b)
{
   struct rect r;
   r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
   r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
   r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
   r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
   return r;
}

static int rect_eq(struct rect a, struct rect b)
{
   return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

/* LookupPaletteSource. */
static int lookup(const struct target* t, int n, u32 cbp, u32 cpsm, u32 cswizzle)
{
   int pass, i;
   for (pass = 0; pass < 2; pass++)
      for (i = 0; i < n; i++)
      {
         struct rect clut;
         int ox, oy;
         if (pass == 0)
         {
            if (t[i].tbp != cbp || t[i].psm != cpsm)
               continue;
            ox = 0;
            oy = 0;
         }
         else
         {
            if (!(t[i].tbp < cbp && t[i].end >= cbp) || t[i].swizzle != cswizzle || !t[i].contains_ok)
               continue;
            ox = t[i].ox;
            oy = t[i].oy;
         }
         clut.x0 = ox;
         clut.y0 = oy;
         clut.x1 = ox + 16;
         clut.y1 = oy + 16;
         if (!rect_eq(rect_and(t[i].valid, clut), clut) || rect_empty(rect_and(t[i].drawn, clut)))
            continue;
         if (!rect_empty(rect_and(t[i].dirty, clut)))
            continue;
         return i;
      }
   return -1;
}

/* The single-pass form, inside-target mode: first match, dirty test only. */
static int lookup_old(const struct target* t, int n, u32 cbp, u32 cpsm)
{
   int i;
   for (i = 0; i < n; i++)
   {
      struct rect clut;
      int ox, oy;
      if (t[i].tbp == cbp)
      {
         if (t[i].psm != cpsm)
            continue;
         ox = 0;
         oy = 0;
      }
      else if (t[i].tbp < cbp && t[i].end >= cbp && t[i].contains_ok)
      {
         ox = t[i].ox;
         oy = t[i].oy;
      }
      else
         continue;
      clut.x0 = ox;
      clut.y0 = oy;
      clut.x1 = ox + 16;
      clut.y1 = oy + 16;
      if (!rect_empty(rect_and(t[i].dirty, clut)))
         continue;
      return i;
   }
   return -1;
}

static int check_lookup(void)
{
   static const struct rect none = { 0, 0, 0, 0 };
   static const struct rect page = { 0, 0, 640, 32 };
   static const struct rect clut_area = { 16, 0, 32, 16 };
   struct target t[2];
   int fail = 0, got, old;

   /* a containing target listed first, drawn; the palette's own target second */
   t[0].tbp = 0x3a00; t[0].end = 0x3cff; t[0].psm = 0; t[0].swizzle = 0; t[0].contains_ok = 1;
   t[0].ox = 16; t[0].oy = 0; t[0].valid = page; t[0].drawn = page; t[0].dirty = none;
   t[1] = t[0];
   t[1].tbp = 0x3ac4; t[1].ox = 0; t[1].oy = 0;
   got = lookup(t, 2, 0x3ac4, 0, 0);
   old = lookup_old(t, 2, 0x3ac4, 0);
   if (got != 1)
   {
      printf("  an exact target lost to a containing one (%d)\n", got);
      fail++;
   }
   if (old == 1)
   {
      printf("  negative: the single pass took the exact target\n");
      fail++;
   }

   /* one containing target, drawn over the palette: it serves */
   t[0].tbp = 0x3ac0;
   if (lookup(t, 1, 0x3ac4, 0, 0) != 0)
   {
      printf("  a drawn containing target does not serve\n");
      fail++;
   }

   /* never drawn there since the last sync: local memory serves */
   t[0].drawn.x0 = 0; t[0].drawn.y0 = 16; t[0].drawn.x1 = 640; t[0].drawn.y1 = 32;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  an undrawn area was read from the target\n");
      fail++;
   }
   if (lookup_old(t, 1, 0x3ac4, 0) != 0)
   {
      printf("  negative: the single pass left the undrawn area alone\n");
      fail++;
   }
   t[0].drawn = clut_area;

   /* laid out otherwise (a 16-bit target under a 32-bit palette) */
   t[0].swizzle = 1;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a target of another layout served the palette\n");
      fail++;
   }
   if (lookup_old(t, 1, 0x3ac4, 0) != 0)
   {
      printf("  negative: the single pass checked the layout\n");
      fail++;
   }
   t[0].swizzle = 0;

   /* the EE wrote over it, or the target does not hold it */
   t[0].dirty = clut_area;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a dirty palette was read from the target\n");
      fail++;
   }
   t[0].dirty = none;
   t[0].valid.x1 = 24;
   if (lookup(t, 1, 0x3ac4, 0, 0) != -1)
   {
      printf("  a palette past the valid area was read from the target\n");
      fail++;
   }
   return fail;
}

/* ---- 3. the palette follows the load ------------------------------------- */

struct tex0
{
   u32 tbp, cbp, csa;
};

struct gs
{
   struct tex0 env[2], prev[2];
   int prim_ctxt, prev_ctxt, backed_up_ctxt, dirty, queued;
   u32 clut_cbp;     /* the last load's CBP: GSClut's m_write */
   u32 drawn_cbp[16]; /* per flushed draw: the CBP its registers carried */
   u32 drawn_load[16]; /* per flushed draw: the CBP of the palette it has */
   int draws;
};

static void flush(struct gs* g)
{
   if (!g->queued)
      return;
   /* GSState::Flush: a dirty state draws from the saved registers */
   g->drawn_cbp[g->draws] = g->dirty ? g->prev[g->prev_ctxt].cbp : g->env[g->prim_ctxt].cbp;
   g->drawn_load[g->draws] = g->clut_cbp;
   g->draws++;
   g->queued = 0;
   if (g->dirty)
      g->backed_up_ctxt = -1;
}

/* ApplyTEX0 with a load: flush, load, then the dirty mask (TBP..TFX, CPSM,
 * CSA; not CBP) when the write is to the drawing context. */
static void write_tex0(struct gs* g, int ctx, struct tex0 t)
{
   flush(g);
   g->clut_cbp = t.cbp;
   g->env[ctx] = t;
   if (ctx == g->prev_ctxt)
      g->dirty = (g->prev[ctx].tbp != t.tbp || g->prev[ctx].csa != t.csa);
}

static void write_other(struct gs* g)
{
   g->dirty = 1;
}

/* VertexKick: the first vertex saves the registers if they changed. */
static void kick(struct gs* g, int ctx)
{
   g->prim_ctxt = ctx;
   if (!g->queued && (g->backed_up_ctxt != ctx || g->dirty))
   {
      g->prev[0] = g->env[0];
      g->prev[1] = g->env[1];
      g->prev_ctxt = ctx;
      g->dirty = 0;
      g->backed_up_ctxt = ctx;
   }
   g->queued = 1;
}

static int check_follows_load(void)
{
   /* eight stripes through eight palettes of one page, then the next page's
    * first pass on the other context, after a register it dirties */
   static const u32 cbp[8] = { 0x3ac0, 0x3ac4, 0x3ad0, 0x3ad4, 0x3ac8, 0x3acc, 0x3ad8, 0x3adc };
   struct gs g;
   struct tex0 t;
   int i, fail = 0, stale = 0;
   g.env[0].tbp = g.env[0].cbp = g.env[0].csa = 0;
   g.env[1] = g.env[0];
   g.prev[0] = g.env[0];
   g.prev[1] = g.env[1];
   g.prim_ctxt = g.prev_ctxt = 0;
   g.backed_up_ctxt = -1;
   g.dirty = g.queued = g.draws = 0;
   g.clut_cbp = 0;
   for (i = 0; i < 8; i++)
   {
      t.tbp = 0x3ae4;
      t.cbp = cbp[i];
      t.csa = 0;
      write_tex0(&g, 1, t);
      kick(&g, 1);
   }
   write_other(&g);
   t.tbp = 0x11a0;
   t.cbp = 0x3ae0;
   t.csa = 0;
   write_tex0(&g, 0, t);
   for (i = 0; i < g.draws; i++)
   {
      if (g.drawn_load[i] != cbp[i])
      {
         printf("  stripe %d has palette %x, wanted %x\n", i, g.drawn_load[i], cbp[i]);
         fail++;
      }
      stale += g.drawn_cbp[i] != cbp[i];
   }
   if (g.draws != 8)
   {
      printf("  %d stripes drawn\n", g.draws);
      fail++;
   }
   /* the registers the last stripe drew from name another palette */
   if (!stale)
   {
      printf("  negative: every draw carried its own CBP\n");
      fail++;
   }
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_sampled_palette();
   fail += check_lookup();
   fail += check_follows_load();
   printf("clut from target: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
