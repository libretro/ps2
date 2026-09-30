/* Who drains the GS ring when a thread asks to wait for it.
 *
 * One rule, in one place, because it has been got wrong three times by
 * being inferred from things that do not determine it:
 *
 *   - which thread opened the GS. Under threaded video the frontend
 *     calls create_device and context_reset on its video thread, so the
 *     opener is a thread that never drains anything;
 *   - which thread last ran MainLoop. The EE reaches it through a wait
 *     at boot, and then owns a ring it must never run;
 *   - a claim made in retro_run. context_reset resumes the EE, so the
 *     EE is already waiting before the first retro_run, and the first
 *     thing that retro_run does is wait as well.
 *
 * What does determine it is which threads produce. The EE and the MTVU
 * worker write the ring and must never run it: the GS holds a hardware
 * context that is not current on them, and a vsync calls the frontend's
 * video callback. Every other caller is on the frontend's side -- its
 * main thread, or its video thread inside a context callback while the
 * main thread is blocked on it -- and a wait from there can only be
 * served by draining, because nothing else will.
 *
 * C, no dependencies: tests/mtgsown includes this and runs the threads.
 */
#ifndef PCSX2_MTGS_OWNER_H
#define PCSX2_MTGS_OWNER_H

#include <stdint.h>

/* self:     the calling thread
 * producer: the EE thread, 0 while there is none
 * is_mtvu:  the call is from the MTVU worker
 * Returns nonzero when the caller must drain the ring itself, zero when
 * it must park until the ring has been drained for it. */
static int mtgs_wait_drains(uintptr_t self, uintptr_t producer, int is_mtvu)
{
	if (is_mtvu)
		return 0;
	return self != producer;
}

/* Whether a vsync taken off the ring is scanned out and handed to the
 * frontend.
 *
 * In retro_run, always: that is the frame. In a drain (flush_all) it is
 * when the thread draining is the one that renders, because since the EE
 * runs a frame ahead of the scanout a drain finds that frame's vsync in
 * the ring - a savestate or a reset taken between two retro_runs would
 * otherwise swallow it.
 *
 * Except while the frontend is taking the context away. context_destroy
 * pauses the EE, which is a drain, and the frame it finds has nowhere to
 * go: the context it would be shown on is the one being destroyed. And
 * presenting it is a call back into the frontend from inside the
 * frontend's own teardown. RetroArch up to 1.22 holds its context lock
 * across context_destroy and takes the same lock at the top of every
 * video_refresh; the lock is not recursive, so the present never returns
 * and neither does the frontend (closing content hung, issue #171).
 *
 * flush_all: the call is a drain, not retro_run's pass
 * self:      the calling thread
 * renderer:  the thread that renders (mtgs_claim_ring)
 * held:      the frontend is taking the context away (mtgs_hold_present) */
static int mtgs_vsync_presents(int flush_all, uintptr_t self,
		uintptr_t renderer, int held)
{
	if (!flush_all)
		return 1;
	if (held)
		return 0;
	return self == renderer;
}

/* Defined in MTGS.cpp. Plain C names and linkage: callable from a C
 * file the day the callers are one. */
#ifdef __cplusplus
extern "C" {
#endif
void mtgs_claim_ring(void);            /* frontend, first thing in retro_run */
void mtgs_set_producer_thread(int on); /* EE thread, as it starts and ends   */
void mtgs_hold_present(int on);        /* frontend, around a context teardown */
#ifdef __cplusplus
}
#endif

#endif
