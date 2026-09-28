/* Models of the paraLLEl-GS sample grids and of what sample_circuit.frag
 * reads from them for one output pixel, shared by pgs_field_scanout.c
 * (which pins them against the grid geometry) and pgs_scanout_exec.c
 * (which runs the shipped shader against them). C89. */
#ifndef PGS_SCANOUT_MODEL_H
#define PGS_SCANOUT_MODEL_H

/* compute_sample_points, in quarter pixels (the grid before rescaling). */
static void sample_point(unsigned i, unsigned rx, unsigned ry, unsigned *x, unsigned *y)
{
   if (ry - rx == 2)
   {
      static const unsigned sparse[4] = { 0, 2, 3, 1 };
      *y = i % 8;
      *x = (i / 8) * 4 + sparse[i % 4];
      return;
   }
   *y = (i & 1) + ((i >> 2) & 1) * 2 + ((i >> 4) & 1) * 4;
   *x = ((i >> 1) & 1) + ((i >> 3) & 1) * 2;
   if (ry - rx == 1)
   {
      *x = *x * 2;
      /* The ordered 4x8 grid keeps its columns in line; the
       * checkerboards stagger them. */
      if (!(rx == 2 && ry == 3))
         *x += i % 2;
   }
}

/* The ordered 4x4 grid at 4x both ways: the one layer under output
 * pixel (gx, gy) of a native pixel. */
static unsigned grid16_layer(unsigned gx, unsigned gy)
{
   return (gy & 1u) | ((gx & 1u) << 1u) | ((gy >> 1u) << 2u) | ((gx >> 1u) << 3u);
}

/* 2x across and 4x over the field: the layers an output pixel (gx, gy)
 * reads on the 8x checkerboard (one) and the ordered 4x4 grid (two,
 * averaged). */
static unsigned field_layers(unsigned samples, unsigned gx, unsigned gy, unsigned *layers)
{
   if (samples == 8)
   {
      layers[0] = (gy & 1u) | (gx << 1u) | ((gy >> 1u) << 2u);
      return 1;
   }
   layers[0] = (gy & 1u) | ((gy >> 1u) << 2u) | (gx << 3u);
   layers[1] = layers[0] + 2u;
   return 2;
}

/* The 4x8 grid: the layers an output pixel (gx, gy) reads at scanout
 * factors (sx, sy), averaged when more than one. */
static unsigned grid32_layers(unsigned sx, unsigned sy, unsigned gx, unsigned gy, unsigned *layers)
{
   if (sx == 2)
   {
      const unsigned column = ((gx & 1u) << 1u) | ((gx >> 1u) << 3u);
      if (sy == 3)
      {
         layers[0] = column | (gy & 1u) | (((gy >> 1u) & 1u) << 2u) | ((gy >> 2u) << 4u);
         return 1;
      }
      layers[0] = column | ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
      layers[1] = layers[0] + 1u;
      return 2;
   }
   else
   {
      const unsigned column = (gx & 1u) << 3u;
      unsigned row, rows, i, n = 0;
      if (sy == 2)
      {
         row = ((gy & 1u) << 2u) | ((gy >> 1u) << 4u);
         rows = 2;
      }
      else
      {
         row = (gy & 1u) << 4u;
         rows = 4;
      }
      for (i = 0; i < rows; i++)
      {
         const unsigned r = row + (i & 1u) + ((i >> 1u) << 2u);
         layers[n++] = column + r;
         layers[n++] = column + r + 2u;
      }
      return n;
   }
}

/* The layers one output pixel reads for a grid of `samples` at factors
 * (sx, sy), on the paths the shader has for them; 0 for a shape it
 * does not scan out this way. (gx, gy) is the output pixel within its
 * native pixel. */
static unsigned scanout_layers(unsigned samples, unsigned sx, unsigned sy, unsigned gx, unsigned gy, unsigned *layers)
{
   if (samples == 16 && sx == 2 && sy == 2)
   {
      layers[0] = grid16_layer(gx, gy);
      return 1;
   }
   if ((samples == 8 || samples == 16) && sx == 1 && sy == 2)
      return field_layers(samples, gx, gy, layers);
   if (samples == 32 && ((sx == 2 && (sy == 2 || sy == 3)) || (sx == 1 && (sy == 1 || sy == 2))))
      return grid32_layers(sx, sy, gx, gy, layers);
   if (samples >= 4 && sx == 1 && sy == 1)
   {
      /* 2x both ways on the other grids: a quarter of the samples,
       * averaged, the quadrant's order swapped on the checkerboard. */
      const unsigned per_quad = samples / 4;
      const unsigned quad = samples != 8 ? (gy & 1u) + (gx & 1u) * 2u : (gx & 1u) + (gy & 1u) * 2u;
      unsigned i;
      for (i = 0; i < per_quad; i++)
         layers[i] = per_quad * quad + i;
      return per_quad;
   }
   return 0;
}

#endif
