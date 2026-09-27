/*
 * The target budget (GSTargetBudget.c): over it, what goes is what the
 * game can no longer read - targets drawn over since, then those unused
 * for a frame, the longest unused first - and never the live set.
 *
 * The live set here is a cutscene's: two 640x448 frame buffers, the
 * picture on screen and the one being drawn, the first blended back into
 * the second each frame, and ten small shadow targets whose textures are
 * as tall as the depth buffer they are drawn with. All used every frame,
 * at native they come to over the budget. Dropping from the back of the
 * cache's lists, the oldest-first order the cache kept, drops a frame
 * buffer: the next frame has nothing to show, and the screen keeps the
 * last picture it had.
 */

#include <stdio.h>

#include "../../pcsx2/GS/Renderers/HW/GSTargetBudget.c"

#define MB ((uint64_t)1 << 20)

static uint64_t sum_bytes(const gs_target_info_t *t, unsigned n)
{
   uint64_t s = 0;
   unsigned i;
   for (i = 0; i < n; i++)
      s += t[i].bytes;
   return s;
}

/* The order the cache dropped in before: from the back of the list
 * (least recently moved to the front), whatever each holds, until the
 * rest fits. */
static unsigned evict_from_back(const gs_target_info_t *t, unsigned n,
      uint64_t usage, uint64_t budget, unsigned *order)
{
   unsigned count = 0;
   while (usage > budget && count < n)
   {
      order[count] = n - 1 - count;
      usage -= t[order[count]].bytes;
      count++;
   }
   return count;
}

static int check_cutscene(void)
{
   /* Index 0 and 11 are the frame buffers, at the list's two ends as a
    * frame leaves them: drawn to at its start and its end. */
   gs_target_info_t t[12];
   unsigned order[12];
   const uint64_t budget = 8 * MB;
   unsigned i, n;
   int fail = 0;

   for (i = 0; i < 12; i++)
   {
      t[i].bytes      = 1146880; /* 640x448 and 64x4480, 32-bit */
      t[i].age        = 1;
      t[i].superseded = 0;
   }

   n = gs_target_evict_order(t, 12, sum_bytes(t, 12), budget, order);
   if (n != 0)
   {
      printf("  the live set lost %u targets, the first %u\n", n, order[0]);
      fail++;
   }

   /* Negative control: from the back, a frame buffer goes. */
   n = evict_from_back(t, 12, sum_bytes(t, 12), budget, order);
   for (i = 0; i < n; i++)
      if (order[i] == 11 || order[i] == 0)
         break;
   if (i == n)
   {
      printf("  negative control: dropping from the back kept both frame buffers\n");
      fail++;
   }
   return fail;
}

static int check_order(void)
{
   /* A: live. B: unused three frames. C: drawn over, live age.
    * D: unused two frames. E: unused ten frames. */
   gs_target_info_t t[5];
   unsigned order[5];
   unsigned n;
   int fail = 0;

   t[0].bytes = 4 * MB; t[0].age = 0;  t[0].superseded = 0;
   t[1].bytes = 4 * MB; t[1].age = 3;  t[1].superseded = 0;
   t[2].bytes = 4 * MB; t[2].age = 1;  t[2].superseded = 1;
   t[3].bytes = 4 * MB; t[3].age = 2;  t[3].superseded = 0;
   t[4].bytes = 4 * MB; t[4].age = 10; t[4].superseded = 0;

   /* Everything that can go, in order. */
   n = gs_target_evict_order(t, 5, 20 * MB, 0, order);
   if (n != 4 || order[0] != 2 || order[1] != 4 || order[2] != 1 || order[3] != 3)
   {
      printf("  order: %u targets, %u %u %u %u\n", n, order[0], order[1], order[2], order[3]);
      fail++;
   }

   /* Only what the budget needs: 20 MB against 12 MB is two. */
   n = gs_target_evict_order(t, 5, 20 * MB, 12 * MB, order);
   if (n != 2 || order[0] != 2 || order[1] != 4)
   {
      printf("  to fit 12 MB: %u targets, %u %u\n", n, order[0], order[1]);
      fail++;
   }

   /* Under it, nothing. */
   n = gs_target_evict_order(t, 5, 20 * MB, 20 * MB, order);
   if (n != 0)
   {
      printf("  at the budget: %u targets dropped\n", n);
      fail++;
   }
   return fail;
}

int main(void)
{
   int fail = check_cutscene() + check_order();
   printf("the target budget keeps the live set: %s\n", fail ? "FAIL" : "ok");
   return fail != 0;
}
