/* Retiring a readback region only once the readback happened.
 *
 * GSTextureCache::Read walks nine paths that write nothing at all: no texture,
 * an EE write overlapping the region, an empty or fully clamped rectangle, an
 * empty write mask, a download texture that will not allocate, map or flush,
 * and a target PSM it has no case for. Its callers clear m_drawn_since_read
 * afterwards. If the region is retired on a call that wrote nothing, the drawn
 * contents never reach local memory and nothing will ask for them again -- so
 * a later indexed source built from that memory is built from whatever was
 * there before, permanently.
 *
 * That matters most where local memory is read back as narrower elements than
 * the target holds: a PSMT8 lookup out of a PSMCT32 target turns every word
 * into four palette indices, so stale words do not blend into the picture,
 * they become four unrelated colours.
 *
 * This runs frames against a model of that loop -- the GPU draws, a readback
 * is attempted, the region is retired -- with the readback refusing on the
 * frames an EE write overlaps it. The retire rule under test is run beside the
 * unconditional one as a negative control, so the lane is shown detecting the
 * loss as well as clearing it.
 *
 * Build and run, from tests/gstc:
 *   cc -O2 -std=c89 -pedantic -Wall readback_retire.c -o readback_retire
 *   ./readback_retire
 */
#include <stdio.h>

#define MEM 64

struct world
{
   unsigned char local[MEM]; /* local memory, what a source is built from */
   unsigned char gpu[MEM];   /* the target's contents */
   int drawn_lo, drawn_hi;   /* m_drawn_since_read, empty when lo >= hi */
};

static void world_init(struct world *w)
{
   int i;
   for (i = 0; i < MEM; i++)
   {
      w->local[i] = 0xAA; /* whatever was there before */
      w->gpu[i]   = 0xAA;
   }
   w->drawn_lo = 0;
   w->drawn_hi = 0;
}

/* The GPU draws into the target and grows the drawn region. */
static void draw(struct world *w, int lo, int hi, unsigned char v)
{
   int i;
   for (i = lo; i < hi; i++)
      w->gpu[i] = v;

   if (w->drawn_lo >= w->drawn_hi)
   {
      w->drawn_lo = lo;
      w->drawn_hi = hi;
   }
   else
   {
      if (lo < w->drawn_lo)
         w->drawn_lo = lo;
      if (hi > w->drawn_hi)
         w->drawn_hi = hi;
   }
}

/* GSTextureCache::Read. Returns whether anything was written. */
static int read_back(struct world *w, int refuse)
{
   int i;
   if (refuse)
      return 0;
   if (w->drawn_lo >= w->drawn_hi)
      return 0;
   for (i = w->drawn_lo; i < w->drawn_hi; i++)
      w->local[i] = w->gpu[i];
   return 1;
}

/* retire_on_write selects the rule: 1 retires only after a write, 0 retires
 * unconditionally, which is the control. */
static void frame(struct world *w, int lo, int hi, unsigned char v,
      int refuse, int retire_on_write)
{
   int wrote;
   draw(w, lo, hi, v);
   wrote = read_back(w, refuse);
   if (wrote || !retire_on_write)
   {
      w->drawn_lo = 0;
      w->drawn_hi = 0;
   }
}

/* How many bytes of what the GPU drew never reached local memory. */
static int lost(const struct world *w)
{
   int i, n = 0;
   for (i = 0; i < MEM; i++)
      if (w->local[i] != w->gpu[i])
         n++;
   return n;
}

static int run(int retire_on_write, const char *label)
{
   /* Each frame draws a different strip, the way a game builds a texture a
    * piece at a time, so a strip that is retired without being written is
    * never revisited. An EE write overlaps the region on the frames marked 1,
    * so the readback refuses there. */
   static const int refuse[] = { 0, 1, 0, 1, 1, 0, 0, 1, 0, 0 };
   struct world w;
   int f;

   world_init(&w);
   for (f = 0; f < 10; f++)
   {
      int lo = f * 4;
      frame(&w, lo, lo + 4, (unsigned char)(0x10 + f), refuse[f], retire_on_write);
   }

   /* A settled frame at the end with nothing in the way. Under the rule being
    * tested the outstanding strips are still pending and get flushed here;
    * under the control they were retired unwritten and are gone. */
   frame(&w, 40, 44, 0x5A, 0, retire_on_write);

   printf("  %-28s bytes still stale: %d\n", label, lost(&w));
   return lost(&w);
}

int main(void)
{
   int failures = 0;
   int kept, control;

   setvbuf(stdout, NULL, _IONBF, 0);

   printf("Readback region retirement, over frames where the readback refuses.\n\n");

   kept    = run(1, "retire only after a write");
   control = run(0, "control: retire always");

   printf("\n");
   if (kept != 0)
   {
      printf("FAIL: drawn contents never reached local memory.\n");
      failures++;
   }
   if (control == 0)
   {
      printf("CONTROL FOUND NOTHING -- the lane is not sensitive to this fault.\n");
      failures++;
   }

   /* A refusal on the very last frame must leave the region outstanding
    * rather than retired, whatever happened on the frames before it. */
   {
      struct world w;
      world_init(&w);
      draw(&w, 0, 16, 0x77);
      if (read_back(&w, 1) != 0)
      {
         printf("FAIL: a refused readback claimed to have written.\n");
         failures++;
      }
      if (w.drawn_lo >= w.drawn_hi)
      {
         printf("FAIL: a refused readback left no region outstanding.\n");
         failures++;
      }
   }

   printf("%s\n", failures == 0 ? "PASS" : "FAIL");
   return failures == 0 ? 0 : 1;
}
