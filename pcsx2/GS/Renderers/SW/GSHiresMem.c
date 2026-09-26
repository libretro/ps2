#include <math.h>
#include <string.h>

#include "GSHiresMem.h"

void gs_hr_layout_init(gs_hr_layout_t *l)
{
   int count[32];
   int x, y;
   const int epb = 256 / l->elem_bytes;

   memset(count, 0, sizeof(count));

   for (y = 0; y < l->ph; y++)
   {
      for (x = 0; x < l->pw; x++)
      {
         const int e = l->swrow[y] + l->swcol[x];
         const int b = e / epb;
         l->blockpix[b][count[b]++] = (unsigned short)(y * l->pw + x);
         l->inv[e]                  = (unsigned short)(y * l->pw + x);
      }
   }
}

unsigned gs_hr_row(const gs_hr_layout_t *l, unsigned page, unsigned bw, int Y)
{
   const int      y  = Y >> 1;
   const int      PY = 2 * (y % l->ph) + (Y & 1);
   const int      qy = PY >= l->ph;
   const unsigned S  = (unsigned)l->page_elems;
   return 4u * S * (page + (unsigned)(y / l->ph) * bw)
      + (qy ? 2u * S : 0u)
      + (unsigned)l->swrow[PY - qy * l->ph];
}

unsigned gs_hr_col(const gs_hr_layout_t *l, int X)
{
   const int      x  = X >> 1;
   const int      PX = 2 * (x % l->pw) + (X & 1);
   const int      qx = PX >= l->pw;
   const unsigned S  = (unsigned)l->page_elems;
   return 4u * S * (unsigned)(x / l->pw)
      + (qx ? S : 0u)
      + (unsigned)l->swcol[PX - qx * l->pw];
}

unsigned gs_hr_elem_mask(const gs_hr_layout_t *l)
{
   return 4u * GS_HR_PAGES * (unsigned)l->page_elems - 1u;
}

unsigned gs_hr_super_elem(const gs_hr_layout_t *l, int PX, int PY)
{
   const int qx = PX >= l->pw;
   const int qy = PY >= l->ph;
   return (unsigned)((qy * 2 + qx) * l->page_elems
      + l->swrow[PY - qy * l->ph] + l->swcol[PX - qx * l->pw]);
}

void gs_hr_pages_reset(gs_hr_pages_t *s)
{
   int i;
   for (i = 0; i < GS_HR_PAGES; i++)
   {
      s->stale[i] = 0xffffffffu;
      s->psm[i]   = 0xff;
   }
}

void gs_hr_mark_block(gs_hr_pages_t *s, unsigned block)
{
   s->stale[(block >> 5) & (GS_HR_PAGES - 1)] |= 1u << (block & 31);
}

int gs_hr_page_needs(const gs_hr_pages_t *s, unsigned page, unsigned psm, unsigned mask)
{
   return s->psm[page] != psm || (s->stale[page] & mask) != 0;
}

/* Every pixel of the block on its 2x2. */
static void gs_hr_replicate_block(const gs_hr_layout_t *l, const void *vm,
      void *hr, unsigned page, unsigned block)
{
   const int      epb    = 256 / l->elem_bytes;
   const unsigned base_n = page * (unsigned)l->page_elems;
   const unsigned base_h = 4u * base_n;
   int k;

   for (k = 0; k < epb; k++)
   {
      const int      pix = l->blockpix[block][k];
      const int      px  = pix % l->pw;
      const int      py  = pix / l->pw;
      const unsigned n   = base_n + (unsigned)(l->swrow[py] + l->swcol[px]);
      const unsigned h00 = base_h + gs_hr_super_elem(l, 2 * px,     2 * py);
      const unsigned h01 = base_h + gs_hr_super_elem(l, 2 * px + 1, 2 * py);
      const unsigned h10 = base_h + gs_hr_super_elem(l, 2 * px,     2 * py + 1);
      const unsigned h11 = base_h + gs_hr_super_elem(l, 2 * px + 1, 2 * py + 1);

      if (l->elem_bytes == 4)
      {
         const unsigned v = ((const unsigned*)vm)[n];
         unsigned      *d = (unsigned*)hr;
         d[h00] = v;
         d[h01] = v;
         d[h10] = v;
         d[h11] = v;
      }
      else
      {
         const unsigned short v = ((const unsigned short*)vm)[n];
         unsigned short      *d = (unsigned short*)hr;
         d[h00] = v;
         d[h01] = v;
         d[h10] = v;
         d[h11] = v;
      }
   }
}

void gs_hr_page_refresh(gs_hr_pages_t *s, const gs_hr_layout_t *l,
      unsigned psm, unsigned page, unsigned mask, const void *vm, void *hr)
{
   unsigned todo;
   unsigned b;

   if (s->psm[page] != psm)
   {
      s->psm[page]   = (unsigned char)psm;
      s->stale[page] = 0xffffffffu;
   }

   todo            = s->stale[page] & mask;
   s->stale[page] &= ~todo;

   for (b = 0; todo; b++, todo >>= 1)
      if (todo & 1)
         gs_hr_replicate_block(l, vm, hr, page, b);
}

void gs_hr_read(const gs_hr_layout_t *l, const void *hr, unsigned page,
      unsigned bw, int x0, int y0, int w, int h, int kind, unsigned ta0,
      unsigned ta1, unsigned *out, int out_pitch, unsigned *colbuf)
{
   const unsigned mask = gs_hr_elem_mask(l);
   const unsigned a0   = ta0 << 24;
   const unsigned a1   = ta1 << 24;
   int X, Y;

   for (X = 0; X < w; X++)
      colbuf[X] = gs_hr_col(l, (2 * x0 + X) & 4095);

   for (Y = 0; Y < h; Y++)
   {
      const unsigned row = gs_hr_row(l, page, bw, (2 * y0 + Y) & 4095);
      unsigned      *d   = (unsigned*)((unsigned char*)out + (size_t)Y * (size_t)out_pitch);

      if (kind == GS_HR_READ_16)
      {
         const unsigned short *s = (const unsigned short*)hr;
         for (X = 0; X < w; X++)
         {
            const unsigned c = s[(row + colbuf[X]) & mask];
            d[X] = ((c << 3) & 0xf8u) | ((c << 6) & 0xf800u) | ((c << 9) & 0xf80000u)
               | ((c & 0x8000u) ? a1 : a0);
         }
      }
      else
      {
         const unsigned *s = (const unsigned*)hr;
         if (kind == GS_HR_READ_24)
         {
            for (X = 0; X < w; X++)
               d[X] = (s[(row + colbuf[X]) & mask] & 0xffffffu) | a0;
         }
         else
         {
            for (X = 0; X < w; X++)
               d[X] = s[(row + colbuf[X]) & mask];
         }
      }
   }
}

void gs_hr_read_texels(const gs_hr_layout_t *l, const gs_hr_pages_t *s,
      unsigned psm, const void *hr, const unsigned *addr,
      const unsigned *native, int n, int sy, int kind, int aem,
      unsigned ta0, unsigned ta1, unsigned *out)
{
   const unsigned S   = (unsigned)l->page_elems;
   const unsigned epb = 256u / (unsigned)l->elem_bytes;
   const unsigned a0  = ta0 << 24;
   const unsigned a1  = ta1 << 24;
   int i, j;

   for (i = 0; i < n; i++)
   {
      const unsigned a    = addr[i] & (GS_HR_PAGES * S - 1u);
      const unsigned page = a / S;
      const unsigned pix  = l->inv[a % S];
      const int      px   = (int)(pix % (unsigned)l->pw);
      const int      py   = (int)(pix / (unsigned)l->pw);
      const unsigned base = 4u * page * S;

      if (s->psm[page] != psm || (s->stale[page] & (1u << ((a / epb) & 31))))
      {
         out[2 * i]     = native[i];
         out[2 * i + 1] = native[i];
         continue;
      }

      for (j = 0; j < 2; j++)
      {
         const unsigned e = base + gs_hr_super_elem(l, 2 * px + j, 2 * py + sy);
         unsigned c;

         if (kind == GS_HR_READ_16)
         {
            c = ((const unsigned short*)hr)[e];
            c = ((c << 3) & 0xf8u) | ((c << 6) & 0xf800u) | ((c << 9) & 0xf80000u)
               | ((aem && c == 0) ? 0u : ((c & 0x8000u) ? a1 : a0));
         }
         else
         {
            c = ((const unsigned*)hr)[e];
            if (kind == GS_HR_READ_24)
            {
               c &= 0xffffffu;
               c |= (aem && c == 0) ? 0u : a0;
            }
         }
         out[2 * i + j] = c;
      }
   }
}

void gs_hr_snap_sprite(float *p0, float *p1, float *t0, float *t1)
{
   const float a = *p0, b = *p1;
   const float na = (float)ceil(a), nb = (float)ceil(b);
   const float u0 = *t0, u1 = *t1;

   if (a == b || (na == a && nb == b))
      return;
   *t0 = u0 + (na - a) * (u1 - u0) / (b - a);
   *t1 = u1 + (nb - b) * (u1 - u0) / (b - a);
   *p0 = na;
   *p1 = nb;
}

void gs_hr_fit_nearest(float p0, float p1, float *t0, float *t1)
{
   const float texel = 65536.0f;
   /* the vertex the sprite starts from along the axis, and its far one */
   float *tf = p1 > p0 ? t0 : t1;
   float *te = p1 > p0 ? t1 : t0;
   const float n = (float)fabs(p1 - p0);
   float u0, du, f0, f1, c0, start, step;

   if (n < 1.0f)
      return;
   u0    = *tf / texel;
   du    = (*te - *tf) / (texel * n);
   f0    = (float)floor(u0);
   f1    = (float)floor(u0 + (n - 1.0f) * du);
   c0    = du >= 0.0f ? 0.25f : 0.75f;
   start = f0 + c0;
   /* 2x pixels 0 and 2n - 1 of the sprite; its far vertex is at 2n */
   step  = ((f1 + 1.0f - c0) - start) / (2.0f * n - 1.0f);
   *tf   = start * texel;
   *te   = (start + step * 2.0f * n) * texel;
}

unsigned gs_hr_wrap_axis(unsigned wm, unsigned size, unsigned minc,
      unsigned maxc, int lo, int hi, unsigned short *tmin,
      unsigned short *tmax, unsigned *mask)
{
   int a, b;

   if (size > 1024)
      size = 1;

   if (lo <= 0 && hi >= (int)size && wm == GS_HR_REPEAT)
   {
      *tmin = (unsigned short)(2 * size - 1);
      *tmax = 0;
      *mask = 0xffffffffu;
      return GS_HR_REPEAT;
   }

   a = 0;
   b = (int)(2 * size - 1);
   if (wm == GS_HR_REGION_CLAMP)
   {
      a = 2 * (int)(minc < size - 1 ? minc : size - 1);
      b = 2 * (int)(maxc < size - 1 ? maxc : size - 1) + 1;
   }
   if (a < 2 * lo)
      a = 2 * lo;
   if (b > 2 * hi - 1)
      b = 2 * hi - 1;
   if (b < a)
      b = a;

   *tmin = (unsigned short)a;
   *tmax = (unsigned short)b;
   *mask = 0;
   return GS_HR_REGION_CLAMP;
}
