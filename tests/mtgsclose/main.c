/* What a drain does with the frame the EE had ready, when the drain is
 * the frontend taking the context away.
 *
 * Since the EE goes on to the next frame as soon as its vsync is
 * consumed (tests/mtgsvsync), it is a frame ahead between two
 * retro_runs: the ring holds that frame and its vsync, and the EE is
 * parked behind it. context_destroy pauses the EE, which drains the
 * ring on the frontend's thread, and the drain used to scan that frame
 * out and hand it over - a video_refresh from inside the frontend's
 * context teardown. RetroArch up to 1.22 holds its context lock across
 * context_destroy and takes it at the top of every video_refresh; the
 * lock is not recursive, so closing content hung the frontend for good
 * (issue #171).
 *
 * Modelled on the real work eventcount, with a producer thread as the
 * EE and this thread as the frontend, which holds a "context lock"
 * across its context_destroy the way that frontend does. Its
 * video_refresh takes the lock with a trylock, so the relock is a
 * counted failure here rather than the hang it is there.
 *
 * What must hold:
 *   - every retro_run presents its frame;
 *   - a drain outside a teardown (a savestate between two retro_runs)
 *     presents the frame it finds, so the frame is not lost;
 *   - the teardown's drain finds a frame - the EE really was ahead -
 *     and does not present it.
 *
 * RULE selects what decides whether a drained vsync is presented:
 *   0  mtgs_vsync_presents (MTGSOwner.h), the rule in the tree
 *   1  the rule it replaced: a drain on the thread that renders always
 *      presents. Re-enters the frontend under its context lock.
 *   2  a drain never presents. Does not re-enter, and loses the frame
 *      at every savestate.
 * build.sh runs 0 and requires 1 and 2 to fail. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <retro_inline.h>
#include <rthreads/rthreads.h>

typedef int32_t  s32;
typedef uint32_t u32;
#include "WorkEventCount.h"
#include "MTGSOwner.h"

#ifndef RULE
#define RULE 0
#endif
#define FRAMES     200
#define SAVESTATES 50

s32 WorkEventCount_SpinBudget(void) { return 0; } /* every wait parks */

static WorkEventCount     ev;
static retro_atomic_int_t ring_work;    /* GS packets in the "ring"       */
static retro_atomic_int_t ring_vsyncs;  /* vsync packets in it            */
static retro_atomic_int_t posted;       /* vsyncs the EE has posted       */
static retro_atomic_int_t pause_req;    /* VMManager::SetPaused           */
static retro_atomic_int_t ee_paused;    /* cpu_thread_state == Paused     */
static retro_atomic_int_t ee_quit;
static uintptr_t producer, renderer;

/* The frontend's side, all on this thread. */
static slock_t *context_lock;  /* held across context_destroy            */
static int held;               /* mtgs_hold_present                      */
static int presented;          /* video_refresh calls                    */
static int relocked;           /* ... made with the context lock held    */
static int consumed;           /* vsyncs taken off the ring              */

static void on_alarm(int sig)
{
	static const char msg[] = "  FAIL: hang -- a wait on the GS never returned\n";
	(void)sig;
	if (write(1, msg, sizeof(msg) - 1) < 0) { }
	_exit(1);
}

/* video_refresh, as that frontend enters it. */
static void video_refresh(void)
{
	if (!slock_try_lock(context_lock))
	{
		relocked++;
		return;
	}
	presented++;
	slock_unlock(context_lock);
}

static int presents(int flush_all)
{
	uintptr_t self = sthread_get_current_thread_id();
#if RULE == 0
	return mtgs_vsync_presents(flush_all, self, renderer, held);
#elif RULE == 1
	(void)mtgs_vsync_presents;
	return !flush_all || self == renderer;
#else
	(void)self;
	(void)mtgs_vsync_presents;
	return !flush_all;
#endif
}

/* MTGS::MainLoop. */
static void drain(int flush_all)
{
	for (;;)
	{
		if (flush_all)
		{
			if (!work_eventcount_check(&ev))
				return;
		}
		else if (!work_eventcount_wait_timed(&ev, 100))
			return;
		while (retro_atomic_load_acquire_int(&ring_work) > 0)
			retro_atomic_fetch_sub_int(&ring_work, 1);
		while (retro_atomic_load_acquire_int(&ring_vsyncs) > 0)
		{
			retro_atomic_fetch_sub_int(&ring_vsyncs, 1);
			consumed++;
			/* The per-frame exit lets the EE go before the scanout. */
			if (!flush_all && work_eventcount_check(&ev))
				work_eventcount_notify(&ev);
			if (presents(flush_all))
				video_refresh();
			if (!flush_all)
				return;
		}
	}
}

/* The EE: a frame's packets, its vsync, and the wait behind it
 * (PostVsyncStart), with the pause taken where Counters.cpp takes it. */
static void ee_thread(void *u)
{
	(void)u;
	for (;;)
	{
		retro_atomic_fetch_add_int(&ring_work, 8);
		work_eventcount_notify(&ev);
		retro_atomic_fetch_add_int(&ring_vsyncs, 1);
		retro_atomic_fetch_add_int(&posted, 1);
		work_eventcount_notify(&ev);
		work_eventcount_wait_empty(&ev);
		if (retro_atomic_load_acquire_int(&pause_req))
		{
			retro_atomic_store_release_int(&ee_paused, 1);
			while (retro_atomic_load_acquire_int(&pause_req))
			{
				if (retro_atomic_load_acquire_int(&ee_quit))
					return;
				sthread_yield();
			}
			retro_atomic_store_release_int(&ee_paused, 0);
		}
	}
}

/* Between two retro_runs: the EE has posted the next frame's vsync. */
static void wait_ee_ahead(void)
{
	while (retro_atomic_load_acquire_int(&posted) <= consumed)
		sthread_yield();
}

/* cpu_thread_pause / cpu_thread_resume (libretro/main.cpp). */
static void pause_ee(void)
{
	retro_atomic_store_release_int(&pause_req, 1);
	while (!retro_atomic_load_acquire_int(&ee_paused))
		drain(1);
}

static void resume_ee(void)
{
	retro_atomic_store_release_int(&pause_req, 0);
	while (retro_atomic_load_acquire_int(&ee_paused))
		sthread_yield();
}

int main(void)
{
	sthread_t *ee;
	int f, s, bad = 0;
	int before, before_consumed, in_teardown;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("mtgsclose (rule %d)\n", RULE);
	signal(SIGALRM, on_alarm);
	alarm(20);

	work_eventcount_init(&ev);
	context_lock = slock_new();
	renderer     = sthread_get_current_thread_id(); /* mtgs_claim_ring */
	ee           = sthread_create(ee_thread, NULL);
	(void)producer;
	(void)mtgs_wait_drains; /* the header's other rule: tests/mtgsown */

	/* retro_run: a frame each. */
	for (f = 0; f < FRAMES; f++)
		drain(0);
	if (presented != FRAMES)
	{
		printf("  FAIL: %d retro_runs presented %d frames\n", FRAMES, presented);
		bad = 1;
	}

	/* A savestate between two retro_runs: a drain, no teardown. The
	 * frame it finds is presented, here rather than in the retro_run
	 * that would have found it. */
	before          = presented;
	before_consumed = consumed;
	for (s = 0; s < SAVESTATES; s++)
	{
		wait_ee_ahead();
		pause_ee();
		resume_ee();
		drain(0);
	}
	if (presented - before != consumed - before_consumed)
	{
		printf("  FAIL: %d vsyncs consumed over %d savestates and %d frames"
				" presented: a drain outside a teardown swallowed the frame"
				" it found\n", consumed - before_consumed, SAVESTATES,
				presented - before);
		bad = 1;
	}

	/* context_destroy, as RetroArch 1.22 calls it. */
	wait_ee_ahead();
	before      = presented;
	in_teardown = consumed;
	slock_lock(context_lock);
	held = 1;                 /* mtgs_hold_present(1) */
	pause_ee();
	held = 0;
	slock_unlock(context_lock);
	in_teardown = consumed - in_teardown;

	if (in_teardown < 1)
	{
		printf("  FAIL: the teardown's drain found no frame: the EE was not"
				" ahead, and this no longer tests anything\n");
		bad = 1;
	}
	if (relocked)
	{
		printf("  FAIL: video_refresh entered %d time(s) with the frontend's"
				" context lock held - on RetroArch 1.22 that call never"
				" returns\n", relocked);
		bad = 1;
	}
	if (presented != before)
	{
		printf("  FAIL: %d frame(s) presented from inside context_destroy\n",
				presented - before);
		bad = 1;
	}

	retro_atomic_store_release_int(&ee_quit, 1);
	sthread_join(ee);
	slock_free(context_lock);

	if (!bad)
		printf("  ok: %d frames, %d savestates kept their frame, and the"
				" teardown consumed %d vsync(s) without a present\n",
				FRAMES, SAVESTATES, in_teardown);
	return bad;
}
