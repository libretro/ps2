/* The texture the frontend is showing outlives a state load.
 *
 * The hardware renderer hands the frontend its merge target (or a
 * deinterlacer's) every VSync, and the frontend keeps showing that image
 * on its own: while the core is paused it replays the last frame it was
 * given. A savestate load or a reset arrives in exactly that state, and
 * GSDevice::ClearCurrent used to free the merge target there and then,
 * so the next replay sampled a destroyed image view (a null dereference
 * inside the Vulkan driver, under RetroArch's own frame). ClearCurrent
 * now retires those targets the way a resized present texture is
 * retired: kept until the frontend has waited through every sync slot
 * since, then freed by AgePool.
 *
 * This models the ring (RetirePresentTexture / AgePool) and runs the
 * pause-then-load sequence through it, once as the code now behaves and
 * once as it did, so the lane shows the difference it makes.
 *
 * Build and run, from tests/gspresent:
 *   cc -O2 -std=c89 -pedantic -Wall present_lifetime.c -o present_lifetime
 *   ./present_lifetime
 */
#include <stdio.h>

#define SLOTS 8       /* NUM_RETIRED_PRESENT_TEXTURES */
#define SYNC_SLOTS 3  /* what the frontend usually offers */

struct device
{
   int alive[64];             /* texture id -> still allocated */
   int retired[SLOTS];        /* texture id or -1 */
   unsigned retired_waits[SLOTS];
   unsigned slot;
   unsigned sync_waits;
   int merge;                 /* the merge target, or -1 */
   int shown;                 /* what the frontend was last handed */
};

static void device_init(struct device* d)
{
   unsigned i;
   for (i = 0; i < 64; i++)
      d->alive[i] = 0;
   for (i = 0; i < SLOTS; i++)
      d->retired[i] = -1;
   d->slot = 0;
   d->sync_waits = 0;
   d->merge = -1;
   d->shown = -1;
}

static void tex_free(struct device* d, int t)
{
   if (t >= 0)
      d->alive[t] = 0;
}

/* GSDevice::RetirePresentTexture */
static void retire(struct device* d, int t)
{
   unsigned s;
   if (t < 0)
      return;
   s = d->slot;
   d->slot = (s + 1) % SLOTS;
   tex_free(d, d->retired[s]);
   d->retired[s] = t;
   d->retired_waits[s] = d->sync_waits;
}

/* GSDevice::AgePool, the retired part */
static void age(struct device* d)
{
   unsigned i;
   for (i = 0; i < SLOTS; i++)
   {
      if (d->retired[i] < 0)
         continue;
      if (d->sync_waits - d->retired_waits[i] > SYNC_SLOTS)
      {
         tex_free(d, d->retired[i]);
         d->retired[i] = -1;
      }
   }
}

/* One VSync on the Vulkan path: wait for the sync index, hand over the
 * merge target, age the ring. */
static void present(struct device* d, int next_id)
{
   d->sync_waits++;
   if (d->merge < 0)
   {
      d->merge = next_id;
      d->alive[next_id] = 1;
   }
   d->shown = d->merge;
   age(d);
}

/* GSDevice::ClearCurrent as it is now, and as it was. */
static void clear_current(struct device* d, int old_way)
{
   if (old_way)
      tex_free(d, d->merge);
   else
      retire(d, d->merge);
   d->merge = -1;
}

static int run(int old_way, int* leaked)
{
   struct device d;
   int i, id = 0;
   int bad = 0;

   device_init(&d);
   for (i = 0; i < 5; i++)
      present(&d, id++);

   /* Pause: the frontend replays what it was handed; nothing presents. */
   /* Savestate load lands here. */
   clear_current(&d, old_way);

   /* The frontend replays the paused frame. */
   if (!d.alive[d.shown])
      bad++;

   /* Play resumes; the ring must let go of it eventually. */
   for (i = 0; i < SYNC_SLOTS + 2; i++)
      present(&d, id++);
   *leaked = d.alive[d.shown] && d.shown != d.merge;
   return bad;
}

int main(void)
{
   int leaked = 0, fail = 0;
   int now = run(0, &leaked);
   int before;

   printf("frontend's frame survives the load: %s\n", now ? "FAIL" : "ok");
   fail += now;
   if (leaked)
   {
      printf("  retired frame never freed\n");
      fail++;
   }
   before = run(1, &leaked);
   printf("control (freed on the spot) is seen failing: %s\n", before ? "ok" : "FAIL");
   if (!before)
      fail++;
   return fail ? 1 : 0;
}
