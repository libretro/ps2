/* paraLLEl-GS: a render pass flushed with an empty last instance.
 *
 * A render pass instance is added before its first primitive grows its
 * bounding box, which starts as the empty box { INT32_MAX, INT32_MAX,
 * INT32_MIN, INT32_MIN }. A flush that lands between the two carries that
 * instance, and GSInterface::flush_render_pass drops it; the binning cost
 * and the coarse tile counts are then taken over the instances that
 * remain, so no width is taken from the empty box (INT32_MIN - INT32_MAX
 * overflows int).
 *
 * Pinned here: the instances sized are the ones with a box, the empty one
 * left out, and every width and height taken fits.
 *
 * Build and run, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_empty_instance.c -o pgs_empty_instance
 *   ./pgs_empty_instance
 */
#include <stdio.h>

#define I32_MAX 2147483647L
#define I32_MIN (-I32_MAX - 1L)

struct bb
{
   long x, y, z, w;
};

/* flush_render_pass: the instances it sizes, the empty last one left out. */
static unsigned sized_instances(const struct bb *inst, unsigned n)
{
   if (n && inst[n - 1].z < 0)
      n--;
   return n;
}

static int check(const struct bb *inst, unsigned n, unsigned want)
{
   const unsigned count = sized_instances(inst, n);
   int fail = 0;
   unsigned i;
   if (count != want)
   {
      printf("  %u instances sized, wanted %u\n", count, want);
      fail++;
   }
   for (i = 0; i < count; i++)
   {
      /* the int subtraction the flush does, checked in long */
      const long dw = inst[i].z - inst[i].x, dh = inst[i].w - inst[i].y;
      if (dw < 0 || dh < 0 || dw > I32_MAX || dh > I32_MAX)
      {
         printf("  instance %u: width %ld height %ld\n", i, dw, dh);
         fail++;
      }
   }
   return fail;
}

int main(void)
{
   static const struct bb with_empty[3] = {
      { 0, 0, 639, 447 }, { 0, 0, 255, 255 }, { I32_MAX, I32_MAX, I32_MIN, I32_MIN }
   };
   static const struct bb full[2] = { { 0, 0, 639, 447 }, { 16, 16, 31, 31 } };
   int fail = 0;
   fail += check(with_empty, 3, 2);
   fail += check(full, 2, 2);
   printf("empty instance: %s\n", fail ? "FAIL" : "ok");
   return fail ? 1 : 0;
}
