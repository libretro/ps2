/*  WorkSema contract, on the eventcount-backed implementation.
 *
 *  The old WorkSema was a hand-rolled state machine; this one is two
 *  libretro-common eventcounts. Same header, same seven operations, and
 *  MTGS and MTVU depend on the exact contract, so this pins each part:
 *
 *    1. NotifyOfWork then CheckForWork returns true once, then false.
 *    2. WaitForWork returns at once when work is pending.
 *    3. WaitForWork parks with nothing pending and wakes on notify.
 *    4. WaitForEmpty returns only after the worker has consumed every
 *       notify so far and gone idle -- including a notify that lands
 *       between the worker's last check and its park.
 *    5. WaitForWorkTimed returns false only on a timeout with nothing
 *       pending, true whenever work arrived.
 *    6. Kill makes every wait return and WaitForEmpty report false;
 *       Reset revives.
 *    7. A producer/consumer pair run hard, with WaitForEmpty interleaved,
 *       loses no wake and never sees WaitForEmpty return while work is
 *       outstanding.
 *
 *  Run under TSan: the two threads share nothing but the primitive.
 *
 *  As with every eventcount test in this series, the lost-wake half of 7
 *  is only load-bearing on more than one core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/Threading.h"
extern "C" {
#include <rthreads/rthreads.h>
}

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* ---- 1, 2, 5, 6: single-threaded contract ------------------------- */
static void contract_single(void)
{
	Threading::WorkSema ws;

	CHECK(!ws.CheckForWork(), "fresh sema reports no work");
	ws.NotifyOfWork();
	CHECK(ws.CheckForWork(), "notify makes CheckForWork true");
	CHECK(!ws.CheckForWork(), "a second CheckForWork is false");
	ws.NotifyOfWork(); ws.NotifyOfWork(); ws.NotifyOfWork();
	CHECK(ws.CheckForWork(), "three notifies are one check");
	CHECK(!ws.CheckForWork(), "and consumed together");

	ws.NotifyOfWork();
	ws.WaitForWork();                       /* must not block */
	CHECK(!ws.CheckForWork(), "WaitForWork consumed the pending notify");

	CHECK(!ws.WaitForWorkTimed(5), "timed wait with nothing pending: false");
	ws.NotifyOfWork();
	CHECK(ws.WaitForWorkTimed(5), "timed wait with work pending: true");

	/* "Empty" is the worker having gone idle after consuming, not merely
	 * having consumed: a worker that took work and has not yet checked
	 * again is still processing, and WaitForEmpty must block for it. The
	 * old state machine did exactly this, checked. So the worker goes
	 * idle first -- one CheckForWork that finds nothing -- and only then
	 * is WaitForEmpty immediate. */
	CHECK(!ws.CheckForWork(), "worker drains and finds nothing: idle");
	CHECK(ws.WaitForEmpty(), "idle worker: WaitForEmpty returns at once, true");

	ws.Kill();
	CHECK(!ws.CheckForWork(), "dead: CheckForWork false");
	ws.WaitForWork();                       /* must not block */
	CHECK(ws.WaitForWorkTimed(1000), "dead: timed wait true at once");
	CHECK(!ws.WaitForEmpty(), "dead: WaitForEmpty false");
	ws.Reset();
	CHECK(!ws.CheckForWork(), "reset: alive and idle");
	CHECK(ws.WaitForEmpty(), "reset: WaitForEmpty true");
	/* And the first sequence again: a fresh object is idle. */
	{
		Threading::WorkSema fresh;
		CHECK(fresh.WaitForEmpty(), "fresh object: WaitForEmpty true at once");
	}
	printf("  contract (single thread): %s\n", fails ? "see above" : "ok");
}

/* ---- 3, 4, 7: the pair ---------------------------------------------- */
static Threading::WorkSema g_ws;
static retro_atomic_int_t  g_queue;      /* items the producer has pushed */
static retro_atomic_int_t  g_taken;      /* items the worker has drained  */
static retro_atomic_int_t  g_stop;
static retro_atomic_int_t  g_bad_empty;  /* WaitForEmpty returned with work outstanding */

static void worker(void*)
{
	while (!retro_atomic_load_relaxed_int(&g_stop))
	{
		g_ws.WaitForWork();
		/* Drain: take everything that has been pushed. */
		for (;;)
		{
			int q = retro_atomic_load_acquire_int(&g_queue);
			int t = retro_atomic_load_relaxed_int(&g_taken);
			if (t >= q) break;
			retro_atomic_store_release_int(&g_taken, t + 1);
		}
	}
}

static void contract_pair(void)
{
	sthread_t* th;
	int i, empties = 0;

	retro_atomic_store_relaxed_int(&g_queue, 0);
	retro_atomic_store_relaxed_int(&g_taken, 0);
	retro_atomic_store_relaxed_int(&g_stop, 0);
	retro_atomic_store_relaxed_int(&g_bad_empty, 0);
	th = sthread_create(worker, NULL);

	for (i = 0; i < 20000; i++)
	{
		int q = retro_atomic_load_relaxed_int(&g_queue);
		int burst = 1 + (i % 4), b;
		for (b = 0; b < burst; b++)
		{
			retro_atomic_store_release_int(&g_queue, q + b + 1);
			g_ws.NotifyOfWork();
		}
		/* Every so often, the thing MTGS does at vsync. */
		if ((i % 7) == 0)
		{
			CHECK(g_ws.WaitForEmpty(), "WaitForEmpty true while alive");
			empties++;
			if (retro_atomic_load_acquire_int(&g_taken)
			  != retro_atomic_load_acquire_int(&g_queue))
				retro_atomic_fetch_add_int(&g_bad_empty, 1);
		}
		/* A short pause so the worker actually parks sometimes. */
		if ((i % 50) == 0) usleep(200);
	}
	CHECK(g_ws.WaitForEmpty(), "final WaitForEmpty");
	CHECK(retro_atomic_load_acquire_int(&g_taken)
	   == retro_atomic_load_acquire_int(&g_queue), "final drain complete");
	CHECK(retro_atomic_load_acquire_int(&g_bad_empty) == 0,
	      "WaitForEmpty never returned with work outstanding");

	retro_atomic_store_relaxed_int(&g_stop, 1);
	g_ws.Kill();
	sthread_join(th);
	printf("  pair: %d items, %d WaitForEmpty calls, %d returned early\n",
	       retro_atomic_load_relaxed_int(&g_queue), empties,
	       retro_atomic_load_relaxed_int(&g_bad_empty));
}

int main(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("worksema (eventcount-backed)\n  cpus: %ld%s\n", n,
	       n > 1 ? "" : "  (lost-wake half not load-bearing on one core)");
	contract_single();
	contract_pair();
	printf(fails ? "worksema: FAILED (%d)\n" : "worksema: ok\n", fails);
	return fails != 0;
}
