/* Concurrent page-fault dispatch stress, on the linked implementation.
 *
 * main.c beside this models the algorithm: it was written when the slot
 * table and the recursion flags lived inside the core's HostSys.cpp,
 * where a test could not reach them. They are libretro-common's
 * faulthandler now, which is a library, so this links the real thing
 * and faults at it.
 *
 * Every worker registers, then repeatedly: decommits a page it owns,
 * touches it (the handler commits it), and checks the store landed.
 * Threads beyond the slot table's capacity must fail to register and
 * must NOT have their faults claimed -- that is the case that would
 * otherwise be a silently swallowed crash on a thread the core does not
 * know about.
 *
 * Counters are per-thread and summed at the end: a commit count that
 * does not match the fault count means a fault was dispatched to the
 * wrong slot or lost. Run under TSan for the dispatch itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <faulthandler.h>
#include <retro_atomic.h>
#include <memmap.h>
#include <rthreads/rthreads.h>

#define MAX_WORKERS 8

static unsigned char *g_base;
static size_t         g_page;
static size_t         g_span;          /* bytes per worker */
static int            g_iters = 20000;

struct worker
{
   int   index;
   int   registered;
   int   faults;
   int   mismatches;
   int   unclaimed;
};

static struct worker g_workers[MAX_WORKERS];
static retro_atomic_int_t g_commits;

static bool handler(const retro_fault_info_t *info)
{
   if (info->addr < (uintptr_t)g_base || info->addr >= (uintptr_t)g_base + g_span * MAX_WORKERS)
      return false;
   if (!memcommit((void*)info->addr, g_page))
      return false;
   retro_atomic_fetch_add_int(&g_commits, 1);
   return true;
}

static void worker_run(void *data)
{
   struct worker *w        = (struct worker*)data;
   volatile unsigned char *p = g_base + g_span * w->index;
   int i;

   w->registered = retro_faulthandler_register_thread() ? 1 : 0;
   if (!w->registered)
   {
      /* Beyond the table: this thread must not fault at all, so it does
       * not touch the region. Reaching here at all is the check. */
      w->unclaimed = 1;
      return;
   }
   for (i = 0; i < g_iters; i++)
   {
      unsigned char v = (unsigned char)(i & 0xFF);
      /* strict: the page must become inaccessible, not merely have its
       * contents dropped -- otherwise the store succeeds and no fault
       * is raised, which is what a non-strict decommit does on POSIX. */
      memdecommit((void*)p, g_page, true);
      *p = v;
      w->faults++;
      if (*p != v)
         w->mismatches++;
   }
   retro_faulthandler_unregister_thread();
}

int main(int argc, char **argv)
{
   int nthreads = 3, i, ok = 1, total_faults = 0, total_mis = 0, registered = 0;
   sthread_t *t[MAX_WORKERS];

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc > 1) nthreads = atoi(argv[1]);
   if (argc > 2) g_iters  = atoi(argv[2]);
   if (nthreads < 1) nthreads = 1;
   if (nthreads > MAX_WORKERS) nthreads = MAX_WORKERS;

   g_page = mempagesize();
   g_span = g_page * 4;
   g_base = (unsigned char*)memreserve(g_span * MAX_WORKERS);
   if (!g_base)
   {
      printf("faultstress-linked: memreserve unavailable; skipped\n");
      return 0;
   }
   if (!retro_faulthandler_install(handler))
   {
      printf("faultstress-linked: install failed\n");
      return 1;
   }
   printf("faultstress-linked: %d threads x %d faults, slot table holds %d\n",
          nthreads, g_iters, RETRO_FAULT_MAX_THREADS);

   for (i = 0; i < nthreads; i++)
   {
      g_workers[i].index = i;
      t[i] = sthread_create(worker_run, &g_workers[i]);
   }
   for (i = 0; i < nthreads; i++)
      if (t[i])
         sthread_join(t[i]);

   for (i = 0; i < nthreads; i++)
   {
      registered   += g_workers[i].registered;
      total_faults += g_workers[i].faults;
      total_mis    += g_workers[i].mismatches;
   }
   printf("  registered %d of %d (table %d), %d faults, %d commits, %d mismatches\n",
          registered, nthreads, RETRO_FAULT_MAX_THREADS, total_faults,
          retro_atomic_load_acquire_int(&g_commits), total_mis);
   if (total_mis)                                        { printf("  FAIL: a store did not land after its fault\n"); ok = 0; }
   if (retro_atomic_load_acquire_int(&g_commits) != total_faults) { printf("  FAIL: commits != faults\n"); ok = 0; }
   if (registered != (nthreads < RETRO_FAULT_MAX_THREADS ? nthreads : RETRO_FAULT_MAX_THREADS))
                                                         { printf("  FAIL: registration count\n"); ok = 0; }
   retro_faulthandler_remove(handler);
   memrelease(g_base, g_span * MAX_WORKERS);
   printf(ok ? "faultstress-linked: ok\n" : "faultstress-linked: FAILED\n");
   return ok ? 0 : 1;
}
