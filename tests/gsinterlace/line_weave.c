/* The deinterlacers weave by frame line at scale.
 *
 * The weave, the blend and the motion-adaptive deinterlacer took every
 * other row of the output from each field. At native a row is a frame
 * line, and that is the weave. Drawn at scale a frame line is several
 * rows, and taking rows in turn mixed the two fields inside every line:
 * a game that draws its two fields half a line apart (a menu font whose
 * two fields hold alternate rows of the glyphs) came out with a stripe
 * through every row of text. The shaders now take
 * each line's rows from the field that drew that line, and for a
 * half-height field, which the merge stretched twice into the source,
 * from that line's block of rows in it. At native nothing changes.
 *
 * This is the shaders' row arithmetic (interlace.glsl / interlace.fx:
 * ps_main0 and line_rows), checked against a model of the
 * fields, at native and at scale, for both field layouts.
 *
 * Build and run, from tests/gsinterlace:
 *   cc -O2 -std=c89 -pedantic -Wall line_weave.c -o line_weave -lm
 *   ./line_weave
 */
#include <math.h>
#include <stdio.h>

/* ZrH.w: rows per frame line at scale, positive for half-height fields
 * (stretched twice into the source), negative for full-height fields. */
static int weave_field(double y, double w)
{
   const double yb = y / fabs(w);
   const int l = (int)yb;
   return l & 1;
}

/* line_rows(): the source coordinate an output row reads, as the row
 * itself for a full-height field and, for a half-height one, the row of
 * the first half of its line's block; the output rows between the
 * field's read the edge between two source rows, where the linear
 * sampler gives their mean (the source holds each field row twice). y
 * is the output row's centre (gl_FragCoord.y), the source has the
 * output's height. */
static double source_row(double y, double w)
{
   const double yb = y / fabs(w);
   const int l = (int)yb;
   if (w > 0.0)
   {
      const double place = ((yb - (double)l) * w - 0.5) * 0.5;
      const double row = floor((double)(l >> 1) * w) + floor(place);
      return 2.0 * row + 0.5 + 3.0 * (place - floor(place));
   }
   return y;
}

/* The model: field f holds line k in its rows k*s .. k*s+s-1 (a
 * half-height field), which the merge stretched to rows 2*that. The
 * field's rows are half a frame line apart, so the block spans two frame
 * lines, and the second half of it samples the positions the other
 * field's line holds. The weave must place field f's line k at output
 * rows (2k+f)*s .. +s-1, from the first half of the block: each field
 * row at its output row, and the output row between two field rows as
 * their mean. */
static int check_half(int s, int lines)
{
   int fail = 0;
   int y;
   for (y = 0; y < 2 * lines * s; y++)
   {
      const double yc = y + 0.5;
      const int line = y / s;            /* frame line */
      const int f = line & 1;
      const int k = line >> 1;           /* the field's line */
      const int sub = y - line * s;      /* row within the line */
      /* even rows within the line read field row k*s + sub/2 alone: a
       * merge row's centre; odd rows read the edge between that field
       * row's second merge row and the next field row's first */
      const int frow = k * s + sub / 2;
      const double want = (sub & 1) ? 2.0 * frow + 2.0 : 2.0 * frow + 0.5;
      const double got = source_row(yc, (double)s);
      if (weave_field(yc, (double)s) != f || (s > 1 && fabs(got - want) > 1e-9) || (s == 1 && (got < 2.0 * frow + 0.5 || got > 2.0 * frow + 1.5)))
      {
         printf("  scale %d row %d: field %d coordinate %g, wanted field %d coordinate %g\n",
            s, y, weave_field(yc, (double)s), got, f, want);
         fail++;
      }
   }
   return fail;
}

/* A full-height field holds the whole frame; its lines interleave in
 * place, so the row reads itself and only the field choice changes. */
static int check_full(int s, int lines)
{
   int fail = 0;
   int y;
   for (y = 0; y < lines * s; y++)
   {
      const double yc = y + 0.5;
      const int line = y / s;
      if (weave_field(yc, -(double)s) != (line & 1) || source_row(yc, -(double)s) != yc)
      {
         printf("  scale %d row %d: full-height field %d row %g\n", s, y, weave_field(yc, -(double)s), source_row(yc, -(double)s));
         fail++;
      }
   }
   return fail;
}

int main(void)
{
   int fail = 0;
   int s;

   /* At native the weave is the old one: row y from field y & 1, half
    * field row y / 2 (merge rows y & ~1, +1), full field row y. */
   for (s = 1; s <= 8; s *= 2)
   {
      fail += check_half(s, 224);
      fail += check_full(s, 448);
   }

   /* The two fields of a glyph row: field 0 holds font row 2k, field 1
    * font row 2k+1, each as a block of s rows. Woven by line, the glyph
    * is s rows of one then s rows of the other; woven by row it was a
    * stripe every row. */
   {
      const int S = 4;
      int y, stripes = 0;
      int prev = -1;
      for (y = 0; y < 2 * S; y++)
      {
         const int f = weave_field(y + 0.5, (double)S);
         if (prev >= 0 && f != prev)
            stripes++;
         prev = f;
      }
      if (stripes != 1)
      {
         printf("  the glyph's two rows change field %d times over %d rows\n", stripes, 2 * S);
         fail++;
      }
   }

   /* An edge the game draws at frame row E lies in field 0 at its line
    * E/2 and in field 1, drawn half a line up, at its line (E-1)/2 + 0.5:
    * at scale s the field holds the edge at row E/2*s (field 0) or
    * ((E-1)/2)*s + s/2 (field 1). Woven, both land on output row E*s
    * (field 1's on the line E-1, at its second half, which the weave
    * leaves to field 0's line E). */
   {
      const int s = 4, E = 46;
      int y, first0 = -1, first1 = -1;
      for (y = 0; y < 2 * 224 * s; y++)
      {
         const double yc = y + 0.5;
         const int f = weave_field(yc, (double)s);
         const int frow = (int)floor(source_row(yc, (double)s)) / 2;
         const int edge_row = f ? ((E - 1) / 2) * s + s / 2 : (E / 2) * s;
         if ((y & 1) == 0 && frow >= edge_row)
         {
            if (f == 0 && first0 < 0) first0 = y;
            if (f == 1 && first1 < 0) first1 = y;
         }
      }
      if (first0 != E * s || first1 != (E + 1) * s)
      {
         printf("  an edge at frame row %d lands at output rows %d (field 0) and %d (field 1), wanted %d and %d\n", E, first0, first1, E * s, (E + 1) * s);
         fail++;
      }
   }

   printf("line weave: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
