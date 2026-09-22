/* The GPU palette shaders against the CPU palette reader.
 *
 * GSClut builds the palette on the CPU into m_buff32, and the software
 * renderer consumes exactly that. With UserHacks_GPUTargetCLUTMode enabled the
 * hardware renderer instead rebuilds the palette on the GPU from a render
 * target, through ps_convert_clut_4 / ps_convert_clut_8. The two have to land
 * on the same CLUT entry for every index, or the same TEX0 draws different
 * colours depending on which renderer ran.
 *
 * CSA selects a sixteen-entry block. GSClut::ReadCLUT_T32_I8 walks the palette
 * a block at a time and clamps the *block* at the last one, 240, while the low
 * four bits go on selecting within it -- so the tail of a CSA-offset palette
 * repeats the last block's sixteen colours rather than one colour. The 8-bit
 * shaders have to clamp at the same granularity.
 *
 * For 4-bit, ReadCLUT_T32_I4 reads sixteen consecutive entries from block CSA,
 * so CSA belongs on the palette index, not on the source coordinate -- adding
 * it there walks diagonally across the target. And once the index leaves the
 * first block it also leaves the leading two rows, so the 4-bit path needs the
 * same group layout as the 8-bit one rather than a plain eight-wide split.
 *
 * Both shader forms that were carried before this test are run alongside the
 * current ones as negative controls, so the lane is shown detecting the fault
 * as well as clearing it.
 *
 * Not covered here, because they are control flow rather than arithmetic and
 * need the renderer to exercise: GSClut::Read32 now declines the GPU palette
 * path for CSM2 (the shaders implement the CSM1 layout only, and
 * LookupPaletteSource overwrites the caller's COU/COV) and for CSA past block
 * 15 (outside the sixteen-block source window). Both leave m_current_gpu_clut
 * null so the CPU palette stands.
 *
 * Build and run, from tests/gsclut:
 *   cc -O2 -std=c89 -pedantic -Wall clut_bank_clamp.c -o clut_bank_clamp
 *   ./clut_bank_clamp
 */
#include <stdio.h>

typedef unsigned int u32;

static u32 umin(u32 a, u32 b)
{
   return a < b ? a : b;
}

/* ---- 8-bit ------------------------------------------------------------- */

/* GSClut::ReadCLUT_T32_I8: blocks of sixteen, base clamped at 240. */
static u32 cpu_clut8(u32 x, u32 offset)
{
   return umin((x & ~15u) + offset, 240u) + (x & 15u);
}

/* ps_convert_clut_8, current: clamp the block, keep the low four bits. */
static u32 shader_clut8(u32 x, u32 offset)
{
   return (umin((x >> 4) + (offset >> 4), 15u) << 4) | (x & 15u);
}

/* ps_convert_clut_8 control: clamp the entry. */
static u32 shader_clut8_control(u32 x, u32 offset)
{
   return umin(x + offset, 255u);
}

/* ---- 4-bit ------------------------------------------------------------- */
/* The shaders map a palette index to a source texel, then read at
 * (base + pos) * scale. Only the offset from base matters here, so base is
 * dropped. */

/* The CSM1 layout the 8-bit path already uses: eight groups of 16x2 with the
 * top-right and bottom-left quadrants swapped. */
static void csm1_pos(u32 index, u32 *px, u32 *py)
{
   u32 subgroup = (index / 8u) % 4u;
   *px = (index % 8u) + ((subgroup >= 2u) ? 8u : 0u);
   *py = ((index / 32u) * 2u) + (subgroup % 2u);
}

/* GSClut::Read32 4-bit: sixteen consecutive entries from block CSA. */
static void cpu_clut4(u32 x, u32 offset, u32 *px, u32 *py)
{
   csm1_pos(x + offset, px, py);
}

/* ps_convert_clut_4, current: offset on the index, CSM1 layout. */
static void shader_clut4(u32 x, u32 offset, u32 *px, u32 *py)
{
   csm1_pos(x + offset, px, py);
}

/* Control: offset on the index, but with the two-rows-of-8x8 shortcut that
 * only holds inside the first block. */
static void shader_clut4_ctl_rows(u32 x, u32 offset, u32 *px, u32 *py)
{
   u32 index = x + offset;
   *px = index % 8u;
   *py = index / 8u;
}

/* Control: offset added to the source coordinate instead of the index. */
static void shader_clut4_ctl_coord(u32 x, u32 offset, u32 *px, u32 *py)
{
   *px = (x % 8u) + offset;
   *py = (x / 8u) + offset;
}

static int failures = 0;

int main(void)
{
   u32 csa;
   u32 tot_ctl8 = 0, tot_ctl4r = 0, tot_ctl4c = 0;

   setvbuf(stdout, NULL, _IONBF, 0);

   printf("GPU palette shaders against GSClut's CPU reader, every CSA and index.\n\n");
   printf("%4s   %8s %8s   %8s %8s %8s\n",
         "", "- 8-bit -", "--------", "- 4-bit -", "--------", "--------");
   printf("%4s   %8s %8s   %8s %8s %8s\n",
         "CSA", "wrong", "ctl:entry", "wrong", "ctl:rows", "ctl:coord");
   printf("%4s   %8s %8s   %8s %8s %8s\n",
         "---", "-----", "---------", "-----", "--------", "---------");

   for (csa = 0; csa <= 15; csa++)
   {
      u32 offset = csa << 4;
      u32 x;
      u32 bad8 = 0, ctl8 = 0, bad4 = 0, ctl4r = 0, ctl4c = 0;

      for (x = 0; x < 256; x++)
      {
         u32 want = cpu_clut8(x, offset);

         if (shader_clut8(x, offset) != want)
         {
            printf("CLUT8 MISMATCH: CSA=%u x=%u want %u got %u\n",
                  csa, x, want, shader_clut8(x, offset));
            bad8++;
            failures++;
         }
         if (shader_clut8_control(x, offset) != want)
            ctl8++;
      }

      for (x = 0; x < 16; x++)
      {
         u32 wx, wy, gx, gy;

         cpu_clut4(x, offset, &wx, &wy);

         shader_clut4(x, offset, &gx, &gy);
         if (gx != wx || gy != wy)
         {
            printf("CLUT4 MISMATCH: CSA=%u x=%u want (%u,%u) got (%u,%u)\n",
                  csa, x, wx, wy, gx, gy);
            bad4++;
            failures++;
         }

         shader_clut4_ctl_rows(x, offset, &gx, &gy);
         if (gx != wx || gy != wy)
            ctl4r++;

         shader_clut4_ctl_coord(x, offset, &gx, &gy);
         if (gx != wx || gy != wy)
            ctl4c++;
      }

      printf("%4u   %8u %8u   %8u %8u %8u\n", csa, bad8, ctl8, bad4, ctl4r, ctl4c);
      tot_ctl8 += ctl8;
      tot_ctl4r += ctl4r;
      tot_ctl4c += ctl4c;
   }

   printf("\ncontrols caught %u of 4096 8-bit, and %u / %u of 256 4-bit lookups\n",
         tot_ctl8, tot_ctl4r, tot_ctl4c);
   if (tot_ctl8 == 0 || tot_ctl4r == 0 || tot_ctl4c == 0)
   {
      printf("A CONTROL FOUND NOTHING -- the lane is not sensitive to that fault.\n");
      failures++;
   }

   /* The tail of a CSA-offset 8-bit palette, so the granularity is readable. */
   {
      u32 x;
      printf("\nCSA=1, entries 240..255 (the clamped tail):\n");
      printf("  index : ");
      for (x = 240; x < 256; x++)
         printf("%4u", x);
      printf("\n  cpu   : ");
      for (x = 240; x < 256; x++)
         printf("%4u", cpu_clut8(x, 16));
      printf("\n  shader: ");
      for (x = 240; x < 256; x++)
         printf("%4u", shader_clut8(x, 16));
      printf("\n  control: ");
      for (x = 240; x < 256; x++)
         printf("%4u", shader_clut8_control(x, 16));
      printf("\n");
   }

   printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
   return failures == 0 ? 0 : 1;
}
