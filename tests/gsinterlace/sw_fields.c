/* The software renderer's deinterlacer on half-height fields.
 *
 * A game that draws each field on its own (SMODE2.FFMD) presents a field
 * per frame; the merge draws it twice as tall, each field row on two
 * rows, and the adaptive deinterlacer (GSDevice::Interlace, mode 3) keeps
 * the last four fields whole, one to a quarter of its buffer, then
 * rebuilds the frame from them. The software device does both passes on
 * the CPU (GSDeviceSW::DoInterlace): the field goes into its slot resampled
 * to the slot's height (BobScaled), the rest of the buffer kept, and each
 * output line is rebuilt as interlace.glsl's mad_fields does (MadFields):
 * a line of the current field as it is, a line between them from the
 * previous field where the picture holds still.
 *
 * Pinned here, on a still picture shown as four fields of alternating
 * parity: every frame after the second gives the picture back, line for
 * line, whichever parity is current. The negative control is the slot
 * written as a full-height copy at the slot's offset, with the rest of the
 * buffer cleared, read back as two banks of interleaved lines: half the
 * frames come out shifted down half a screen with black above.
 *
 * Build and run, from tests/gsinterlace:
 *   cc -O2 -std=c89 -pedantic -Wall sw_fields.c -o sw_fields -lm
 *   ./sw_fields
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define H 448            /* frame lines */
#define F (H / 2)        /* rows of a field */

static int picture[H];
static int merge[H];
static int mad[2 * H];
static int out[H];

/* The picture's value at a line: anything that tells lines apart. */
static int line_value(int y)
{
   return 17 + (y * 37) % 211;
}

/* GSDevice::Interlace: the buffer index of the field just stored. */
static int next_index(int idx, int field)
{
   idx++;
   idx &= ~1;
   idx |= field;
   return idx & 3;
}

/* The merge of a half-height field: field row j on rows 2j and 2j + 1. */
static void draw_merge(int parity)
{
   int j;
   for (j = 0; j < F; j++)
      merge[2 * j] = merge[2 * j + 1] = picture[2 * j + parity];
}

/* SampleRowLinear, one channel: position r in rows, clamped to [lo, hi). */
static int sample(const int* src, int lo, int hi, double r)
{
   const double fy = r - 0.5;
   int y0 = (int)floor(fy), y1;
   const unsigned wt = (unsigned)((fy - y0) * 256.0 + 0.5);
   y1 = y0 + 1;
   if (y0 < lo) y0 = lo;
   if (y0 > hi - 1) y0 = hi - 1;
   if (y1 < lo) y1 = lo;
   if (y1 > hi - 1) y1 = hi - 1;
   if (wt == 0 || y0 == y1)
      return src[y0];
   return (int)(((unsigned)src[y0] * (256u - wt) + (unsigned)src[y1] * wt + 128u) >> 8);
}

/* BobScaled into slot idx of the buffer. */
static void store_slot(int idx)
{
   const int dy0 = idx * (H / 2), dy1 = dy0 + H / 2;
   int y;
   for (y = dy0; y < dy1; y++)
      mad[y] = sample(merge, 0, H, ((y - dy0) + 0.5) * ((double)H / (dy1 - dy0)));
}

/* MadFields on a still picture: the motion is zero, so a line between the
 * current field's takes the previous field's. */
static double field_row(int idx, int back, int y)
{
   const int slot_h = H / 2;
   const int slot = (idx - back) & 3;
   const int par = (idx ^ back) & 1;
   double r = (y - par) * 0.5 + 0.5;
   if (r < 0.5) r = 0.5;
   if (r > slot_h - 0.5) r = slot_h - 0.5;
   return sample(mad, slot * slot_h, (slot + 1) * slot_h, slot * slot_h + r);
}

static void rebuild(int idx)
{
   int y;
   for (y = 0; y < H; y++)
      out[y] = (int)field_row(idx, ((y & 1) == (idx & 1)) ? 0 : 1, y);
}

/* The form it replaces: the field copied full height at the slot's offset,
 * the rest cleared (BobCopy), and the buffer read as two banks of H lines,
 * the current bank by idx >> 1 (MadReconstruct on a still picture). */
static void store_slot_old(int idx)
{
   int y;
   for (y = 0; y < 2 * H; y++)
   {
      const int sy = y - idx * (H / 2);
      mad[y] = (sy >= 0 && sy < H) ? merge[sy] : 0;
   }
}

static void rebuild_old(int idx)
{
   static const int t0[4] = { 0, 0, H, H }, t1[4] = { H, 0, 0, H };
   int y;
   for (y = 0; y < H; y++)
      out[y] = ((y & 1) == (idx & 1)) ? mad[y + t0[idx]] : mad[y + t1[idx]];
}

static int run(int old, int* bad_frames)
{
   int n, y, idx = 0, fail = 0;
   *bad_frames = 0;
   memset(mad, 0, sizeof(mad));
   for (n = 0; n < 8; n++)
   {
      const int parity = n & 1;
      int wrong = 0;
      idx = next_index(idx, parity);
      draw_merge(parity);
      if (old)
      {
         store_slot_old(idx);
         rebuild_old(idx);
      }
      else
      {
         store_slot(idx);
         rebuild(idx);
      }
      if (n < 2)
         continue;
      for (y = 0; y < H; y++)
         wrong += out[y] != picture[y];
      if (wrong)
      {
         (*bad_frames)++;
         if (!old)
         {
            printf("  frame %d (field %d, slot %d): %d lines differ from the picture\n", n, parity, idx, wrong);
            fail++;
         }
      }
   }
   return fail;
}

int main(void)
{
   int y, fail, bad, bad_old;
   for (y = 0; y < H; y++)
      picture[y] = line_value(y);

   fail = run(0, &bad);
   run(1, &bad_old);
   if (bad_old < 3)
   {
      printf("  negative: the full-height copy into a slot gave the picture back (%d bad frames)\n", bad_old);
      fail++;
   }
   printf("software deinterlacer, half-height fields: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
