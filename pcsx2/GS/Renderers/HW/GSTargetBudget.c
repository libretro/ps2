#include "GSTargetBudget.h"

/* Live: used in the frame just ended, or in the one being drawn. */
#define GS_TARGET_LIVE_AGE 1

unsigned gs_target_evict_order(const gs_target_info_t *t, unsigned n,
      uint64_t usage, uint64_t budget, unsigned *order)
{
   unsigned count = 0;
   unsigned i;

   while (usage > budget)
   {
      unsigned best = n;

      for (i = 0; i < n; i++)
      {
         unsigned k;

         if (!t[i].superseded && t[i].age <= GS_TARGET_LIVE_AGE)
            continue;
         for (k = 0; k < count; k++)
            if (order[k] == i)
               break;
         if (k < count)
            continue;
         if (best == n
               || t[i].superseded > t[best].superseded
               || (t[i].superseded == t[best].superseded && t[i].age > t[best].age))
            best = i;
      }

      if (best == n)
         break;
      order[count++] = best;
      usage         -= t[best].bytes < usage ? t[best].bytes : usage;
   }

   return count;
}
