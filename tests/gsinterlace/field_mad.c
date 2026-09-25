/* The motion-adaptive deinterlacer on half-height fields.
 *
 * A game that draws each field on its own (SMODE2.FFMD) has, at scale,
 * two rows of each field for every frame line: its line j, drawn at frame
 * line 2j + p for a field of parity p, covers that line and the next. The
 * adaptive deinterlacer keeps the last four fields whole in a buffer of
 * four slots and reconstructs each output row from them
 * (interlace.glsl / interlace.fx: mad_field, mad_fields; GSDevice::
 * Interlace): a line of the current field as it is; a line between them
 * from the previous field where the picture holds still, and from the
 * current field's own rows at that place where it moves, so that a moving
 * picture is the current field's own, whole, and a still one the two
 * fields woven.
 *
 * Pinned here:
 *  - the slot rotation: the slot and parity the shader takes for the
 *    field k back hold the field k back, from either first parity;
 *  - the row placement: every output row reads a row of the field it
 *    takes (current or previous) that covers the row's place in the frame,
 *    at scales 2, 4 and 8, for both parities;
 *  - the current field alone gives its whole picture: each of its rows
 *    twice, in order, none left out; at native, a line between its lines
 *    reads the mean of the two around it;
 *  - one decision per native pixel: every row of a line's block measures
 *    motion at the same rows.
 *
 * Build and run, from tests/gsinterlace:
 *   cc -O2 -std=c89 -pedantic -Wall field_mad.c -o field_mad -lm
 *   ./field_mad
 */
#include <math.h>
#include <stdio.h>

/* GSDevice::Interlace: the buffer index of the field just stored. */
static int next_index(int idx, int field)
{
   idx++;
   idx &= ~1;
   idx |= field;
   return idx & 3;
}

/* mad_field(): the field row coordinate (texel centre at k + 0.5) that
 * output row y (its index) reads from a field of parity par, block rows
 * to a frame line. */
static double field_row(double y, int par, double block, double slot_h)
{
   const double rel = (y - (double)par * block) * 0.5;
   double r = ((block >= 2.0) ? floor(rel) : rel) + 0.5;
   if (r < 0.5)
      r = 0.5;
   if (r > slot_h - 0.5)
      r = slot_h - 0.5;
   return r;
}

static int check_rotation(void)
{
   int fail = 0, first;
   for (first = 0; first < 2; first++)
   {
      int slot_of[4] = { -1, -1, -1, -1 }; /* the field stored in each slot */
      int par_of[64];
      int idx = 0, t;
      for (t = 0; t < 64; t++)
      {
         const int field = (first + t) & 1;
         int back;
         idx = next_index(idx, field);
         slot_of[idx] = t;
         par_of[t] = field;
         for (back = 0; back < 4 && back <= t; back++)
         {
            const int slot = (idx - back) & 3;
            const int par = (idx ^ back) & 1;
            if (slot_of[slot] != t - back || par != par_of[t - back])
            {
               if (fail++ < 4)
                  printf("  field %d, %d back: slot %d holds field %d parity %d\n", t, back, slot, slot_of[slot], par);
            }
         }
      }
   }
   return fail;
}

static int check_placement(void)
{
   static const int scales[] = { 2, 4, 8 };
   const int lines = 448;
   int fail = 0;
   size_t si;
   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int s = scales[si];
      const double slot_h = (double)(lines * s) * 0.5;
      int par;
      for (par = 0; par < 2; par++)
      {
         int y, last = -1, count = 0;
         for (y = 0; y < lines * s; y++)
         {
            /* the row's place in the frame, in frame lines */
            const double top = (double)y / s, bot = (double)(y + 1) / s;
            const double r = field_row((double)y, par, (double)s, slot_h);
            const int k = (int)floor(r);
            /* field row k covers frame lines par + 2k/s .. par + 2(k+1)/s */
            const double ftop = (double)par + 2.0 * k / s, fbot = (double)par + 2.0 * (k + 1) / s;
            const int clamped = (y < par * s) || (k == (int)slot_h - 1 && bot > fbot);
            if (!clamped && (top < ftop - 1e-9 || bot > fbot + 1e-9))
            {
               if (fail++ < 6)
                  printf("  scale %d parity %d row %d reads field row %d over %g..%g\n", s, par, y, k, ftop, fbot);
            }
            if (r - floor(r) != 0.5)
            {
               if (fail++ < 6)
                  printf("  scale %d parity %d row %d reads between rows (%g)\n", s, par, y, r);
            }
            /* the current field alone: each row twice, in order */
            if (!clamped)
            {
               if (k == last)
                  count++;
               else
               {
                  if (last >= 0 && (k != last + 1 || count != 2))
                  {
                     if (fail++ < 6)
                        printf("  scale %d parity %d: field row %d shown %d times, then row %d\n", s, par, last, count, k);
                  }
                  last = k;
                  count = 1;
               }
            }
         }
      }
   }
   /* At native a field row is a frame line: its own line reads it, the
    * line after it the mean of it and the next. */
   {
      const double slot_h = 224.0;
      int par, k;
      for (par = 0; par < 2; par++)
         for (k = 1; k < 200; k++)
         {
            const double own = field_row((double)(par + 2 * k), par, 1.0, slot_h);
            const double between = field_row((double)(par + 2 * k + 1), par, 1.0, slot_h);
            if (own != k + 0.5 || between != k + 1.0)
            {
               if (fail++ < 6)
                  printf("  native parity %d line %d: own %g, between %g\n", par, k, own, between);
            }
         }
   }
   return fail;
}

/* One decision per native pixel: the rows measured are the first rows of
 * the line and of the lines above and below, the same for every row of
 * the block, and the column the block's centre. */
static int check_decision(void)
{
   static const int scales[] = { 1, 2, 4, 8 };
   int fail = 0;
   size_t si;
   for (si = 0; si < sizeof(scales) / sizeof(scales[0]); si++)
   {
      const int s = scales[si];
      int l;
      for (l = 1; l < 100; l++)
      {
         int y, x;
         for (y = l * s; y < (l + 1) * s; y++)
         {
            const int line = (int)((double)y / s);
            const double yl = (double)line * s;
            if (line != l || yl != (double)l * s)
            {
               if (fail++ < 4)
                  printf("  scale %d row %d measures line %d at %g\n", s, y, line, yl);
            }
         }
         for (x = 0; x < 4 * s; x++)
         {
            const double fx = x + 0.5;
            const double xc = (floor(fx / s) + 0.5) * s;
            if (xc != (double)(x / s) * s + 0.5 * s)
            {
               if (fail++ < 4)
                  printf("  scale %d column %d measures at %g\n", s, x, xc);
            }
         }
      }
   }
   return fail;
}

/* Reading row by row, as the lines' own rows blended with the mean of
 * the lines around them, mixes the two fields inside a native line: the
 * stripes along a moving picture. Taking the current field's own rows,
 * a moving line's rows all come from one field. */
static int check_one_field_per_line(void)
{
   const int s = 4, lines = 448;
   const double slot_h = (double)(lines * s) * 0.5;
   int fail = 0, par, y;
   for (par = 0; par < 2; par++)
      for (y = s * 2; y < (lines - 2) * s; y++)
      {
         /* every row of a moving line reads the current field, at a row
          * that lies within the line's own place */
         const double r = field_row((double)y, par, (double)s, slot_h);
         const int k = (int)floor(r);
         const double ftop = (double)par + 2.0 * k / s;
         const int line = y / s;
         if (ftop < (double)line - 1e-9 || ftop + 2.0 / s > (double)(line + 1) + 1e-9)
         {
            if (fail++ < 4)
               printf("  parity %d row %d of line %d reads field row %d at %g\n", par, y, line, k, ftop);
         }
      }
   return fail;
}

int main(void)
{
   int fail = 0;
   fail += check_rotation();
   fail += check_placement();
   fail += check_decision();
   fail += check_one_field_per_line();
   printf("field adaptive deinterlace: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
