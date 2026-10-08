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
 * Nonzero when the caller must drain the ring itself, zero when it must
 * park until the ring has been drained for it. */
#define MTGS_WAIT_DRAINS(self, producer, is_mtvu) \
	(!(is_mtvu) && (self) != (producer))

/* What happens to a vsync, and where the EE stops for a pause.
 *
 * The EE goes on to the next frame as soon as its vsync is consumed, so
 * between two retro_runs it is a frame ahead: running that frame, or
 * parked behind its vsync. retro_run consumes the vsync, scans the frame
 * out and presents it. Anything else that empties the ring - pausing the
 * EE for a savestate, a reset, an option change, a disc swap, the
 * frontend taking the context away - is a drain, and a drain never
 * presents: a present is a call into the frontend's video_refresh, which
 * belongs inside retro_run and nowhere else. The drained vsync is kept in
 * mtgs_pending instead, and the next retro_run presents it rather than
 * waiting for another, so every retro_run is one frame however many
 * drains came between.
 *
 * A pause stops the EE only at a vsync a drain consumed. Asked while the
 * EE runs the frame ahead, it finishes that frame; asked in the moment
 * after retro_run let it go and before it noticed, it runs the frame too
 * rather than stopping at the vsync retro_run already presented. Either
 * way a pause leaves the machine at the same point - one frame ahead,
 * its vsync pending - whatever the timing of the two threads, and that
 * point is what a savestate holds. Anything else that interrupts the EE
 * (stopping, a CPU change) still stops it at once.
 *
 * interrupted: VMManager asks the EE to stop
 * paused:      and what it asks for is a pause
 * drained:     the vsync the EE was let go from was a drain's */
#define MTGS_EE_STOPS(interrupted, paused, drained) \
	((interrupted) && (!(paused) || (drained)))

/* A drained vsync, kept for the next retro_run: the GS's copy of the
 * privileged registers at that vsync, the field it scans out, whether the
 * registers changed since the one before, and the audio mark that ends
 * its frame. Frontend's thread only. */
struct mtgs_frame
{
	uint8_t  regs[0x2000]; /* PS2MEM_GS_REGS */
	uint32_t audio_mark;
	uint32_t field;
	uint32_t registers_written;
	uint32_t pending;
};

/* Defined in MTGS.cpp. Plain C names and linkage: callable from a C
 * file the day the callers are one. */
#ifdef __cplusplus
extern "C" {
#endif
void mtgs_claim_ring(void);            /* frontend, first thing in retro_run */
void mtgs_set_producer_thread(int on); /* EE thread, as it starts and ends   */

/* Frontend's thread, retro_run: scan out and present the drained frame,
 * when there is one. Nonzero when it did. */
int mtgs_present_pending(void);

extern struct mtgs_frame mtgs_pending;
/* The audio mark of the last vsync retro_run consumed. */
extern uint32_t mtgs_presented_mark;
/* Set by whoever consumed the vsync the EE was last let go from: nonzero
 * for a drain. Read by the EE after its wait. */
extern int mtgs_vsync_drained;
#ifdef __cplusplus
}
#endif

#endif
