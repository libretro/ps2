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

#endif
