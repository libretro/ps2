/* When the EE is let go at a vsync, and what the GS scans out after.
 *
 * The EE posts a vsync to the GS ring and waits in WaitGS until the GS
 * has consumed it. MTGS::MainLoop, on the frontend's thread, consumes
 * it, takes its copy of the privileged registers, and goes idle at the
 * current epoch - which is what WaitGS's empty-wait returns on - before
 * it scans the frame out and hands it to the frontend. The EE then runs
 * the next frame while the scanout, the frontend's present and its
 * pacing wait happen; and the scanout reads the registers as they were
 * at the vsync, whatever the EE has written to the live ones since.
 *
 * Modelled here on the real work eventcount (pcsx2/WorkEventCount.h),
 * with a producer thread as the EE and this thread as the frontend. The
 * "present" of each frame waits, with a timeout, for the producer to
 * report that it was released and wrote the next frame's registers; the
 * scanout then reads the copy. Both must hold every frame:
 *   - the producer was released while the frame was being presented;
 *   - the copy the scanout reads holds the vsync's value, not the
 *     next frame's.
 *
 * RULE selects the consumer's order:
 *   0  the tree's: copy, go idle, then scan out and present
 *   1  scan out and present, then commit the vsync and go idle on the
 *      next call's wait
 *   2  go idle, then scan out from the live registers, no copy
 * build.sh runs 0 and requires 1 and 2 to fail: under 1 the producer is
 * held through every present, so no release is ever observed during one;
 * under 2 the scanout reads the next frame's registers.
 *
 * Build, from tests/mtgsvsync: see build.sh. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <retro_inline.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_atomic.h>
#include "common/Pcsx2Types.h"
#include "WorkEventCount.h"

s32 WorkEventCount_SpinBudget(void) { return 0; } /* every wait parks */

#ifndef RULE
#define RULE 0
#endif

#define FRAMES 64
#define PRESENT_TIMEOUT_US 50000

static WorkEventCount ev;

/* The privileged registers: written by the producer, read by the
 * consumer through its copy. One word stands for the display register a
 * game rewrites at every vsync. */
static retro_atomic_int_t live_regs;
#if RULE == 0
static int regs_copy;
#endif

/* The producer's report: which frame it was last released after. The
 * present waits on it with the eventcount. */
static retro_atomic_int_t released_after;
static retro_eventcount_t released_ec;

/* The vsync record in the ring: posted by the producer, committed by
 * the consumer. WaitGS waits for the ring to drain. */
static retro_atomic_int_t ring_vsyncs;

static int ring_drained(void *ctx)
{
   (void)ctx;
   return retro_atomic_load_acquire_int(&ring_vsyncs) == 0;
}

static void producer_main(void *arg)
{
   int frame;
   (void)arg;
   for (frame = 1; frame <= FRAMES; frame++)
   {
      /* The frame's registers, then the vsync, then WaitGS(false). */
      retro_atomic_store_release_int(&live_regs, frame);
      retro_atomic_fetch_add_int(&ring_vsyncs, 1);
      work_eventcount_notify(&ev);
      work_eventcount_wait_drained(&ev, ring_drained, NULL);
      /* Released: the next frame starts with its register write. */
      retro_atomic_store_release_int(&live_regs, frame + 1);
      retro_atomic_store_release_int(&released_after, frame);
      retro_eventcount_notify(&released_ec);
   }
}

/* The frontend's present: it lasts until the producer reports the
 * release for this frame, or the timeout. Returns 1 if the release came
 * during the present. */
static int present(int frame)
{
   for (;;)
   {
      int key;
      if (retro_atomic_load_acquire_int(&released_after) >= frame)
         return 1;
      key = retro_eventcount_prepare_wait(&released_ec);
      if (retro_atomic_load_acquire_int(&released_after) >= frame)
      {
         retro_eventcount_cancel_wait(&released_ec);
         return 1;
      }
      if (!retro_eventcount_commit_wait_timeout(&released_ec, key, PRESENT_TIMEOUT_US))
         return retro_atomic_load_acquire_int(&released_after) >= frame;
   }
}

int main(void)
{
   sthread_t *producer;
   int frame;
   int fail = 0;

   work_eventcount_init(&ev);
   retro_eventcount_init(&released_ec);
   retro_atomic_store_relaxed_int(&live_regs, 0);
   retro_atomic_store_relaxed_int(&released_after, 0);
   retro_atomic_store_relaxed_int(&ring_vsyncs, 0);

   producer = sthread_create(producer_main, NULL);
   if (!producer)
   {
      printf("cannot start the producer\n");
      return 1;
   }

   for (frame = 1; frame <= FRAMES; frame++)
   {
      int during, scanned;

      /* MainLoop: wait for the vsync (the frame's only packet here). */
      while (!work_eventcount_wait_timed(&ev, 100))
         ;

#if RULE == 0
      /* The tree's order: the registers are the frame's while the
       * producer is held; copy them, let it go, then scan out. */
      regs_copy = retro_atomic_load_acquire_int(&live_regs);
      retro_atomic_fetch_sub_int(&ring_vsyncs, 1);
      work_eventcount_drained(&ev, work_eventcount_epoch(&ev), 0);
      during  = present(frame);
      scanned = regs_copy;
#elif RULE == 2
      /* Let go first, then scan out from the live registers. */
      retro_atomic_fetch_sub_int(&ring_vsyncs, 1);
      work_eventcount_drained(&ev, work_eventcount_epoch(&ev), 0);
      during  = present(frame);
      scanned = retro_atomic_load_acquire_int(&live_regs);
#else
      /* Scan out and present first; the producer is let go by the
       * next wait's idle. The scanout reads the live registers. */
      during  = present(frame);
      scanned = retro_atomic_load_acquire_int(&live_regs);
      retro_atomic_fetch_sub_int(&ring_vsyncs, 1);
#endif

      if (!during)
      {
         if (fail < 3)
            printf("  frame %d: the EE was held through the present\n", frame);
         fail++;
      }
      else if (scanned != frame)
      {
         if (fail < 3)
            printf("  frame %d: the scanout read the registers of frame %d\n", frame, scanned);
         fail++;
      }
   }

   /* Under the orders that hold the producer through the present it is
    * still in its last WaitGS: let every wait return before joining. */
   work_eventcount_kill(&ev);
   sthread_join(producer);
   work_eventcount_free(&ev);
   retro_eventcount_free(&released_ec);

   printf("vsync release before scanout: %s (%d of %d frames wrong)\n", fail ? "FAIL" : "ok", fail, FRAMES);
   return fail ? 1 : 0;
}
