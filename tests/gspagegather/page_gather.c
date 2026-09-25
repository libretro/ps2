/* A read of a render target's pages at another buffer width, at the
 * target's own base.
 *
 * A 16-bit buffer 10 pages wide (640 pixels) is drawn with a picture
 * 320x112 split in two: rows 0..63 at x 0..319, rows 64..111 at
 * x 320..639, both in the first page row. Read back 5 pages wide, the GS
 * page addressing puts the second half under the first, and the read is
 * the whole 320x112 picture. The hardware renderer keeps the buffer as
 * one texture laid out at its own width, so read in place it would give
 * the left half and whatever lies below it. Pinned here, against the GS
 * page addressing:
 *
 *   1. The layout itself: the 5-page read of the 10-page buffer is the
 *      picture, and a direct read of the texture is not.
 *   2. The decision to gather the read's pages from the texture: taken
 *      for a read at the target's base, in its format, at another width,
 *      that reaches past the first page row (or past the target's width);
 *      not taken for a read within the first page row, which is in place.
 *      A bilinear read reaches one texel past the rect, which past the
 *      read's width is not a page of the row: that one texel is dropped
 *      before the pages are checked, or a target only one page row tall
 *      would refuse the read; a rect that starts past the width addresses
 *      the pages there and keeps them. A draw flagged as a possible
 *      texture shuffle does not stop the gather of a 16-bit target read as
 *      16-bit (a shuffle reads a 32-bit target); it does for a 32-bit one.
 *      The target has to have been last drawn at its own width, pixel for
 *      pixel: a channel shuffle's emulation writes the picture it moves,
 *      not the pages where the target's width puts them, so a read of a
 *      target it last wrote stays in place.
 *   3. The gather: each page of the read copied from where the target's
 *      width put it, at native and at 4x, gives the picture.
 *   4. The copies made with the target's alpha rescaled take their source
 *      rect normalised on both axes and put it where the plain copy puts
 *      it: normalising only the far edge sampled every page but the first
 *      from outside the texture, clamped to its edge.
 *
 * Build and run, from tests/gspagegather:
 *   cc -O2 -std=c89 -pedantic -Wall page_gather.c -o page_gather
 *   ./page_gather
 */
#include <stdio.h>
#include <string.h>

#define PG 64        /* PSMCT16 page: 64x64 pixels */
#define TBW 10       /* the target's width in pages */
#define RBW 5        /* the read's width in pages */
#define PIC_W 320
#define PIC_H 112
#define MAXS 4       /* largest scale tried */

typedef unsigned short u16;

/* Local memory, as pages of the target's format. */
static u16 mem[32][PG][PG];

static void mem_write(int bw, int x, int y, u16 v)
{
   mem[(y / PG) * bw + x / PG][y % PG][x % PG] = v;
}

static u16 mem_read(int bw, int x, int y)
{
   return mem[(y / PG) * bw + x / PG][y % PG][x % PG];
}

static u16 picture(int x, int y)
{
   return (u16)(y * PIC_W + x + 1);
}

/* The target's texture: its width, as many rows as were drawn, scaled. */
static u16 tex[PG * MAXS][TBW * PG * MAXS];
static int tex_w, tex_h;

static void build_texture(int rows, int s)
{
   int x, y;
   tex_w = TBW * PG * s;
   tex_h = rows * s;
   for (y = 0; y < tex_h; y++)
      for (x = 0; x < tex_w; x++)
         tex[y][x] = mem_read(TBW, x / s, y / s);
}

/* The end of a read's rect, less a filter's one-texel reach past the
 * read's width; a rect that starts past the width keeps its end. */
static int read_end_x(int bw, int rx, int rz)
{
   return (rx < bw * PG && rz == bw * PG + 1) ? bw * PG : rz;
}

/* The decision, as the texture cache takes it. */
static int pages_in_target(int bw, int tbw, int t_w, int t_h, int rx, int ry, int rz, int rw, int clip)
{
   const int px0 = rx / PG, py0 = ry / PG;
   const int px1 = ((clip ? read_end_x(bw, rx, rz) : rz) - 1) / PG, py1 = (rw - 1) / PG;
   int px, py;
   for (py = py0; py <= py1; py++)
      for (px = px0; px <= px1; px++)
      {
         const int n = py * bw + px;
         const int sx = (n % tbw) * PG, sy = (n / tbw) * PG;
         if (sx + PG > t_w || sy + PG > t_h)
            return 0;
      }
   return 1;
}

static int drawn_at_width = 1;

static int gathers(int same_base, int same_psm, int bpp, int bw, int tbw, int t_w, int t_h,
   int rx, int ry, int rz, int rw, int possible_shuffle, int target_32bit)
{
   const int rz_c = read_end_x(bw, rx, rz);
   return same_base && same_psm && bw != tbw && bw > 0 && drawn_at_width && bpp >= 16 &&
      (!possible_shuffle || !target_32bit) &&
      ((rw - 1) / PG > 0 || (rz_c - 1) / PG >= tbw) &&
      pages_in_target(bw, tbw, t_w, t_h, rx, ry, rz, rw, 1);
}

/* A normalised copy of a rect of a texture, sampled at texel centres with
 * the coordinate clamped to the texture, as the device's stretch does. */
static u16 out[PG * 2 * MAXS][RBW * PG * MAXS];

static void stretch(const float s_rect[4], const int d_rect[4])
{
   int x, y;
   for (y = d_rect[1]; y < d_rect[3]; y++)
      for (x = d_rect[0]; x < d_rect[2]; x++)
      {
         const float u = s_rect[0] + (s_rect[2] - s_rect[0]) * ((float)(x - d_rect[0]) + 0.5f) / (float)(d_rect[2] - d_rect[0]);
         const float v = s_rect[1] + (s_rect[3] - s_rect[1]) * ((float)(y - d_rect[1]) + 0.5f) / (float)(d_rect[3] - d_rect[1]);
         int tx = (int)(u * (float)tex_w), ty = (int)(v * (float)tex_h);
         tx = tx < 0 ? 0 : (tx >= tex_w ? tex_w - 1 : tx);
         ty = ty < 0 ? 0 : (ty >= tex_h ? tex_h - 1 : ty);
         out[y][x] = tex[ty][tx];
      }
}

/* The gather: every page of the read from its place in the target. With
 * `whole_axes` the source rect is normalised on both axes. */
static void gather(int s, int whole_axes, int rz, int rw)
{
   const int px1 = (read_end_x(RBW, 0, rz) - 1) / PG, py1 = (rw - 1) / PG;
   int px, py;
   memset(out, 0, sizeof(out));
   for (py = 0; py <= py1; py++)
      for (px = 0; px <= px1; px++)
      {
         const int n = py * RBW + px;
         const int sx = (n % TBW) * PG, sy = (n / TBW) * PG;
         float sr[4];
         int dr[4];
         sr[0] = (float)(sx * s) / (whole_axes ? (float)tex_w : 1.0f);
         sr[1] = (float)(sy * s) / (whole_axes ? (float)tex_h : 1.0f);
         sr[2] = (float)((sx + PG) * s) / (float)tex_w;
         sr[3] = (float)((sy + PG) * s) / (float)tex_h;
         dr[0] = px * PG * s;
         dr[1] = py * PG * s;
         dr[2] = (px + 1) * PG * s;
         dr[3] = (py + 1) * PG * s;
         stretch(sr, dr);
      }
}

static int out_is_picture(int s)
{
   int x, y, bad = 0;
   for (y = 0; y < PIC_H * s; y++)
      for (x = 0; x < PIC_W * s; x++)
         if (out[y][x] != picture(x / s, y / s))
            bad++;
   return bad;
}

int main(void)
{
   int fail = 0;
   int x, y, s, bad;

   /* The picture, drawn in two halves side by side at the target's width. */
   memset(mem, 0xcc, sizeof(mem));
   for (y = 0; y < PG; y++)
      for (x = 0; x < PIC_W; x++)
         mem_write(TBW, x, y, picture(x, y));
   for (y = PG; y < PIC_H; y++)
      for (x = 0; x < PIC_W; x++)
         mem_write(TBW, PIC_W + x, y - PG, picture(x, y));

   /* 1. The layout. */
   bad = 0;
   for (y = 0; y < PIC_H; y++)
      for (x = 0; x < PIC_W; x++)
         if (mem_read(RBW, x, y) != picture(x, y))
            bad++;
   if (bad)
   {
      printf("  the read at %d pages is not the picture at %d texels\n", RBW, bad);
      fail++;
   }
   build_texture(PG, 1);
   bad = 0;
   for (y = 0; y < PIC_H; y++)
      for (x = 0; x < PIC_W; x++)
         if (y >= tex_h || tex[y][x] != picture(x, y))
            bad++;
   if (!bad)
   {
      printf("  a direct read of the texture gives the picture; the case pins nothing\n");
      fail++;
   }

   /* 2. The decision. A bilinear read of the picture: one texel over. */
   if (!gathers(1, 1, 16, RBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 0, 0))
   {
      printf("  the read at another width, past the first page row, is not gathered\n");
      fail++;
   }
   if (pages_in_target(RBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 0))
   {
      printf("  unclipped, the filter's reach should land below a one-row target\n");
      fail++;
   }
   /* A rect that starts past the read's width addresses the pages there
    * (a base a page back, coordinates a page on): nothing is clipped. */
   if (read_end_x(1, PG, PG + PG / 2) != PG + PG / 2 || read_end_x(1, 0, PG + PG / 2) != PG + PG / 2 ||
       read_end_x(RBW, 0, PIC_W + 1) != PIC_W)
   {
      printf("  the clip reaches past a filter's one texel\n");
      fail++;
   }
   if (!gathers(1, 1, 16, RBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 1, 0))
   {
      printf("  a possible shuffle stops the gather of a 16-bit target\n");
      fail++;
   }
   if (gathers(1, 1, 16, RBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 1, 1))
   {
      printf("  a possible shuffle of a 32-bit target is gathered\n");
      fail++;
   }
   if (gathers(1, 1, 16, RBW, TBW, TBW * PG, PG, 0, 0, PIC_W, PG - 8, 0, 0))
   {
      printf("  a read within the first page row is gathered\n");
      fail++;
   }
   bad = 0;
   for (y = 0; y < PG - 8; y++)
      for (x = 0; x < PIC_W; x++)
         if (tex[y][x] != mem_read(RBW, x, y))
            bad++;
   if (bad)
   {
      printf("  a read within the first page row is not in place (%d texels)\n", bad);
      fail++;
   }
   if (gathers(1, 1, 16, TBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 0, 0) ||
       gathers(1, 0, 16, RBW, TBW, TBW * PG, PG, 0, 0, PIC_W + 1, PIC_H + 2, 0, 0))
   {
      printf("  a read at the target's width, or in another format, is gathered\n");
      fail++;
   }

   /* A target last written by a shuffle, whose emulation lays the pages out
    * as the picture it moves rather than at the target's width, is read in
    * place. */
   drawn_at_width = 0;
   if (gathers(1, 1, 32, 2, 8, 512, 448, 0, 0, 128, 448, 0, 1))
   {
      printf("  a read of a target a shuffle last wrote is gathered\n");
      fail++;
   }
   drawn_at_width = 1;

   /* 3 and 4. The gather, at native and at 4x, normalised both ways. */
   for (s = 1; s <= MAXS; s *= 4)
   {
      build_texture(PG, s);
      gather(s, 1, PIC_W + 1, PIC_H + 2);
      bad = out_is_picture(s);
      if (bad)
      {
         printf("  scale %d: the gathered read differs from the picture at %d texels\n", s, bad);
         fail++;
      }
      gather(s, 0, PIC_W + 1, PIC_H + 2);
      if (!out_is_picture(s))
      {
         printf("  scale %d: a source rect normalised on the far edge only should miss\n", s);
         fail++;
      }
   }

   /* 4. The alpha-rescaled twin of a plain copy of an area at an offset
    * to the texture's origin: the same texels at the same place. */
   build_texture(PG, 1);
   {
      const int ax = 64, ay = 16, aw = 64, ah = 32;
      float sr[4];
      int dr[4];
      memset(out, 0, sizeof(out));
      sr[0] = (float)ax / (float)tex_w;
      sr[1] = (float)ay / (float)tex_h;
      sr[2] = (float)(ax + aw) / (float)tex_w;
      sr[3] = (float)(ay + ah) / (float)tex_h;
      dr[0] = 0;
      dr[1] = 0;
      dr[2] = aw;
      dr[3] = ah;
      stretch(sr, dr);
      bad = 0;
      for (y = 0; y < ah; y++)
         for (x = 0; x < aw; x++)
            if (out[y][x] != tex[ay + y][ax + x])
               bad++;
      if (bad)
      {
         printf("  the rescaled copy of an offset area is not the plain copy (%d texels)\n", bad);
         fail++;
      }
   }

   printf("page gather: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
