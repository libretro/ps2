/* REGION_REPEAT, and what the texture cache may crop away.
 *
 * REGION_REPEAT addresses a texel as (u & MINU) | MAXU -- a bitwise
 * mask-and-fix, not a window. GSdx has an exact shader for it
 * (tfx.glsl: "(uvec4(uv * tex_size) & msk) | fix"), but that shader runs only
 * while the texture cache has left the source whole. Once
 * SourceRegion::Create crops one out, EmulateTextureSampler's EffectiveClamp
 * turns REGION_REPEAT into CLAMP_REPEAT, clears ps.wms/ps.wmt and enables the
 * hardware sampler's own repeat over the cropped rectangle
 * [MAXU, MINU|MAXU]. So the crop is only sound where a plain repeat across
 * that rectangle lands on the same texel the mask-and-fix would.
 *
 * Writing rw for the rectangle's width, the bits of MINU that survive the OR
 * with MAXU are exactly the bits below rw, so the two agree for every u iff
 * those bits are a contiguous run -- iff rw is a power of two. Agreeing as
 * sets is not sufficient: bilinear filtering samples neighbours, so the
 * mapping has to match texel for texel.
 *
 * parallel-gs needs no such test. gs_renderer.cpp hands umsk/ufix to the
 * upload shader and halves both per mip level, so texel u of the uploaded
 * image already holds hardware texel (u & MINU) | MAXU and any repeat over it
 * is exact; its compute_effective_texture_extent "LSB > MSB" check only
 * decides how little it is worth uploading. That column is printed below to
 * show it is a size decision and a far narrower one, not the correctness
 * gate.
 *
 * The walk over every (MINU, MAXU) pair is the ground truth here; the
 * power-of-two predicate is checked against it rather than trusted. The gate
 * GSdx carried before this test is run alongside the current one as a
 * negative control, so the table shows the lane detecting the fault as well
 * as clearing it.
 *
 * Build and run, from tests/gsregion:
 *   cc -O2 -std=c89 -pedantic -Wall gs_region_repeat.c -o gs_region_repeat
 *   ./gs_region_repeat
 */
#include <stdio.h>

typedef unsigned int u32;
typedef int s32;

/* ---- transcribed from GSTextureCache.cpp: SourceRegion ------------------- */

struct Region
{
   s32 minx;
   s32 maxx;
   int has;
};

static s32 region_width(const struct Region *r)
{
   return r->maxx - r->minx;
}

/* SourceRegion::Create, REGION_REPEAT branch, X only. "gated" selects the
 * power-of-two requirement; without it this is the negative control. */
static struct Region create_region_repeat(u32 TW, u32 MINU, u32 MAXU, int gated)
{
   struct Region r;
   r.has  = 0;
   r.minx = 0;
   r.maxx = 0;

   if (MINU != 0)
   {
      u32 rw    = ((MINU | MAXU) - MAXU) + 1;
      int sound = !gated || (rw & (rw - 1)) == 0;

      if (sound && (rw < (1u << TW) || (MAXU != 0 && (rw <= (1u << TW)))))
      {
         r.has  = 1;
         r.minx = (s32)MAXU;
         r.maxx = (s32)((MINU | MAXU) + 1);
      }
   }
   return r;
}

/* SourceRegion::AdjustForMipmap, X only. */
static struct Region adjust_for_mipmap(const struct Region *in, u32 level)
{
   struct Region ret;
   ret.has  = 0;
   ret.minx = 0;
   ret.maxx = 0;

   if (in->has)
   {
      s32 w    = region_width(in) >> level;
      if (w < 1)
         w = 1;
      ret.has  = 1;
      ret.minx = in->minx >> level;
      ret.maxx = ret.minx + w;
   }
   return ret;
}

/* ---- the two answers ----------------------------------------------------- */

/* The hardware, and GSdx's own shader path, which is the same expression. */
static u32 addr_hw(u32 u, u32 msk, u32 fix)
{
   return (u & msk) | fix;
}

/* A cropped region sampled with the hardware sampler's repeat. */
static u32 addr_cropped(u32 u, const struct Region *r)
{
   return (u32)r->minx + (u % (u32)region_width(r));
}

/* The closed form the fix is gated on. */
static int crop_is_exact(u32 msk, u32 fix)
{
   u32 rw = ((msk | fix) - fix) + 1;
   return (rw & (rw - 1)) == 0;
}

/* parallel-gs compute_effective_texture_extent, REGION_REPEAT branch: a size
 * decision, not a correctness one. */
static int pgs_shrinks_upload(u32 msk, u32 fix)
{
   u32 msk_msb = 0;
   u32 fix_lsb = 32;
   u32 i;

   if (msk == 0)
      return 1;

   for (i = 0; i < 32; i++)
      if (msk & (1u << i))
         msk_msb = i;

   for (i = 0; i < 32; i++)
   {
      if (fix & (1u << i))
      {
         fix_lsb = i;
         break;
      }
   }

   return fix_lsb > msk_msb;
}

static int failures = 0;

static u32 walk(u32 TW, u32 MINU, u32 MAXU, const struct Region *r)
{
   u32 size = 1u << TW;
   u32 u;

   for (u = 0; u < size; u++)
      if (addr_hw(u, MINU, MAXU) != addr_cropped(u, r))
         return 1;
   return 0;
}

static void worked_example(void)
{
   /* msk 010101, fix 001000 */
   const u32 TW = 6, MINU = 0x15, MAXU = 0x08;
   struct Region c = create_region_repeat(TW, MINU, MAXU, 0);
   u32 u;

   printf("\nworked example  TW=%u  MINU=0x%02x  MAXU=0x%02x   rw=%u, not a power of two\n",
         TW, MINU, MAXU, ((MINU | MAXU) - MAXU) + 1);
   printf("  control crops to [%d, %d), current gate crops: %s\n",
         c.minx, c.maxx,
         create_region_repeat(TW, MINU, MAXU, 1).has ? "yes" : "no");

   printf("  u       : ");
   for (u = 0; u < 12; u++)
      printf("%3u", u);
   printf("\n  hardware: ");
   for (u = 0; u < 12; u++)
      printf("%3u", addr_hw(u, MINU, MAXU));
   printf("\n  cropped : ");
   for (u = 0; u < 12; u++)
      printf("%3u", addr_cropped(u, &c));
   printf("\n");
}

static void mip_levels(void)
{
   u32 TW;

   printf("\nmip levels, over the regions the current gate admits:\n");
   printf("%4s %10s %10s\n", "TW", "regions", "wrong");
   printf("%4s %10s %10s\n", "--", "-------", "-----");

   for (TW = 4; TW <= 10; TW++)
   {
      u32 size = 1u << TW;
      u32 regions = 0, mipwrong = 0;
      u32 MINU, MAXU, level, u;

      for (MINU = 0; MINU < size; MINU++)
      {
         for (MAXU = 0; MAXU < size; MAXU++)
         {
            struct Region r = create_region_repeat(TW, MINU, MAXU, 1);
            if (!r.has)
               continue;
            regions++;

            for (level = 1; level <= 3 && (TW - level) >= 3; level++)
            {
               struct Region rm = adjust_for_mipmap(&r, level);
               u32 msk_m  = MINU >> level;
               u32 fix_m  = MAXU >> level;
               u32 size_m = 1u << (TW - level);

               for (u = 0; u < size_m; u++)
               {
                  if (addr_hw(u, msk_m, fix_m) != addr_cropped(u, &rm))
                  {
                     printf("MIP MISADDRESSES: TW=%u level=%u MINU=0x%x MAXU=0x%x\n",
                           TW, level, MINU, MAXU);
                     mipwrong++;
                     failures++;
                     break;
                  }
               }
            }
         }
      }
      printf("%4u %10u %10u\n", TW, regions, mipwrong);
   }
}

int main(void)
{
   u32 tot_wrong = 0, tot_crop = 0, ctl_wrong = 0, ctl_crop = 0;
   u32 TW;

   setvbuf(stdout, NULL, _IONBF, 0);

   printf("REGION_REPEAT: a cropped region against the (u & MINU) | MAXU the hardware does.\n");
   printf("Every (MINU, MAXU) pair below the texture size, at each TW.\n\n");

   printf("%4s %9s   %19s   %19s %10s\n", "", "", "-- current gate --", "-- control ----", "");
   printf("%4s %9s   %9s %9s   %9s %9s %10s\n",
         "TW", "pairs", "cropped", "wrong", "cropped", "wrong", "pgs-shrink");
   printf("%4s %9s   %9s %9s   %9s %9s %10s\n",
         "--", "-----", "-------", "-----", "-------", "-----", "----------");

   for (TW = 3; TW <= 10; TW++)
   {
      u32 size = 1u << TW;
      u32 pairs = 0, crop = 0, wrong = 0, ccrop = 0, cwrong = 0, pgs = 0;
      u32 MINU, MAXU;

      for (MINU = 0; MINU < size; MINU++)
      {
         for (MAXU = 0; MAXU < size; MAXU++)
         {
            struct Region c, g;

            pairs++;
            if (pgs_shrinks_upload(MINU, MAXU))
               pgs++;

            c = create_region_repeat(TW, MINU, MAXU, 0);
            if (c.has)
            {
               u32 bad = walk(TW, MINU, MAXU, &c);
               ccrop++;
               cwrong += bad;
               /* The predicate must track the walk, or it is not the right
                * thing to gate on. */
               if ((bad != 0) == crop_is_exact(MINU, MAXU))
               {
                  printf("PREDICATE DISAGREES WITH WALK: TW=%u MINU=0x%x MAXU=0x%x\n",
                        TW, MINU, MAXU);
                  failures++;
               }
            }

            g = create_region_repeat(TW, MINU, MAXU, 1);
            if (g.has)
            {
               u32 bad = walk(TW, MINU, MAXU, &g);
               crop++;
               wrong += bad;
               if (bad)
               {
                  printf("CROPPED REGION MISADDRESSES: TW=%u MINU=0x%x MAXU=0x%x\n",
                        TW, MINU, MAXU);
                  failures++;
               }
            }
         }
      }

      printf("%4u %9u   %9u %9u   %9u %9u %10u\n",
            TW, pairs, crop, wrong, ccrop, cwrong, pgs);
      tot_wrong += wrong;
      tot_crop  += crop;
      ctl_wrong += cwrong;
      ctl_crop  += ccrop;
   }

   printf("\ncurrent gate : %u of %u cropped regions misaddress\n", tot_wrong, tot_crop);
   printf("control      : %u of %u cropped regions misaddress\n", ctl_wrong, ctl_crop);
   if (ctl_wrong == 0)
   {
      printf("CONTROL FOUND NOTHING -- the lane is not sensitive to this fault.\n");
      failures++;
   }

   worked_example();
   mip_levels();

   printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
   return failures == 0 ? 0 : 1;
}
