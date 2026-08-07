/* Concurrent page-fault dispatch stress.
 *
 * WHAT THIS TESTS
 *
 * The fault-handler rework replaced a pthread mutex held inside the
 * SIGSEGV handler with (a) a lock-free slot table keyed on thread
 * identity, (b) per-slot recursion flags, and (c) an atomic-flag
 * spinlock inside vtlb guarding the backpatch bookkeeping.
 *
 * None of that was exercised where it was written: the container had
 * one vCPU and MTVU's >=3-hardware-thread gate meant only ONE thread
 * ever faulted.  Every interesting path - two threads in the handler
 * at once, spinlock contention, slot lookup under concurrent
 * register/unregister - is untested by construction there.
 *
 * This program drives exactly those paths: N threads take real
 * SIGSEGV page faults simultaneously, in a handler that reproduces
 * the shipped algorithm (slot table + per-slot recursion flag +
 * spinlock-guarded shared bookkeeping), while a churn thread
 * registers and unregisters to race the lookup.
 *
 * SCOPE - READ THIS
 *
 * This is a test of the ALGORITHM, not of the linked binary: the slot
 * table and spinlock are reproduced here rather than linked from
 * common/HostSys.cpp (which drags in most of the emulator).  It will
 * catch a design flaw - a lost slot, a dropped recursion flag, a
 * spinlock that fails to exclude, a torn shared counter.  It will NOT
 * catch a transcription error in HostSys.cpp itself.
 *
 * The authoritative in-situ test remains: run the emulator with MTVU
 * enabled under ThreadSanitizer on a >=3-hardware-thread host, on
 * content that faults heavily (any game during its memory-clear
 * phases).  This program is the fast, deterministic pre-check.
 *
 * USAGE (from repo root)
 *
 *   sh tests/faultstress/build.sh
 *   ./tests/faultstress/fault_test           # defaults: 4 threads
 *   ./tests/faultstress/fault_test 8 200000  # threads, faults each
 *   TSAN_OPTIONS=halt_on_error=1 ./tests/faultstress/fault_test_tsan 4
 *
 * WHAT FAILURE LOOKS LIKE
 *
 *   - "SLOT LOST" / "NO SLOT"      : slot table lookup failed under churn
 *   - "RECURSION FLAG"             : per-slot flag not mutually exclusive
 *   - "COUNTER MISMATCH"           : spinlock failed to exclude
 *   - TSan data race report        : ordering bug in the protocol
 *   - hang                         : spinlock deadlock (see note below)
 *
 * Exit 0 = clean.
 *
 * NOTE ON SPINLOCK SAFETY: spinning inside a signal handler is only
 * safe because nothing under the lock blocks, allocates, or faults.
 * This test deliberately keeps the critical section to plain memory
 * writes, matching vtlb's actual usage.  If a future change puts a
 * blocking call under that lock, this test will hang - which is the
 * correct alarm.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#define MAX_THREADS         16
#define FAULT_SLOTS         4      /* mirrors FAULT_THREAD_SLOTS */
#define PAGES_PER_THREAD    64

/* ---- shipped algorithm, reproduced ---------------------------------- */

static volatile int  slot_claimed[FAULT_SLOTS];
static volatile long slot_id[FAULT_SLOTS];
static volatile int  slot_inhandler[FAULT_SLOTS];

static uintptr_t thread_identity(void)
{
   return (uintptr_t)pthread_self();
}

static int slot_register(void)
{
   const uintptr_t self = thread_identity();
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
         return i;
   }
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      int expected = 0;
      if (__atomic_exchange_n(&slot_claimed[i], 1, __ATOMIC_ACQ_REL) == expected)
      {
         __atomic_store_n(&slot_inhandler[i], 0, __ATOMIC_RELEASE);
         __atomic_store_n(&slot_id[i], (long)self, __ATOMIC_RELEASE);
         return i;
      }
   }
   return -1;
}

static void slot_unregister(void)
{
   const uintptr_t self = thread_identity();
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
      {
         __atomic_store_n(&slot_id[i], 0L, __ATOMIC_RELEASE);
         __atomic_store_n(&slot_claimed[i], 0, __ATOMIC_RELEASE);
         return;
      }
   }
}

static int slot_lookup(uintptr_t self)
{
   int i;
   for (i = 0; i < FAULT_SLOTS; i++)
   {
      if ((uintptr_t)__atomic_load_n(&slot_id[i], __ATOMIC_ACQUIRE) == self)
         return i;
   }
   return -1;
}

/* vtlb's async-signal-safe spinlock, reproduced. */
static volatile int shared_lock;

static void shared_lock_acquire(void)
{
   while (__atomic_exchange_n(&shared_lock, 1, __ATOMIC_ACQ_REL))
   {
   }
}

static void shared_lock_release(void)
{
   __atomic_store_n(&shared_lock, 0, __ATOMIC_RELEASE);
}

/* ---- shared bookkeeping, guarded by the spinlock -------------------- */

/* Non-atomic ON PURPOSE: correctness must come from the spinlock.  A
 * mismatch at the end proves the lock failed to exclude, and TSan
 * reports the race directly. */
static uint64_t guarded_counter;
static uint64_t guarded_shadow[8];

static volatile sig_atomic_t saw_no_slot;
static volatile sig_atomic_t saw_recursion_conflict;

/* ---- fault machinery ------------------------------------------------ */

static size_t page_size;

struct thread_ctx
{
   int       id;
   uint8_t  *region;      /* PAGES_PER_THREAD pages, PROT_NONE */
   long      faults;
   uint64_t  local_count;
};

static __thread struct thread_ctx *tls_ctx; /* handler needs the region */

static void fault_handler(int sig, siginfo_t *si, void *uctx)
{
   uintptr_t self = thread_identity();
   int       slot;
   int       i;
   (void)sig; (void)uctx;

   slot = slot_lookup(self);
   if (slot < 0)
   {
      saw_no_slot = 1;
      _exit(3);
   }

   /* Per-slot recursion guard: must be exclusive for this slot. */
   if (__atomic_exchange_n(&slot_inhandler[slot], 1, __ATOMIC_ACQ_REL))
   {
      saw_recursion_conflict = 1;
      _exit(4);
   }

   /* Critical section: exactly the shape of vtlb's bookkeeping -
    * plain host-memory writes, no allocation, no blocking, no
    * access to the faulting region. */
   shared_lock_acquire();
   guarded_counter++;
   for (i = 0; i < 8; i++)
      guarded_shadow[i] = guarded_counter + (uint64_t)i;
   shared_lock_release();

   /* "Handle" the fault: make the page accessible so we can resume,
    * mirroring vtlb's backpatch-and-continue. */
   if (mprotect((void*)((uintptr_t)si->si_addr & ~(uintptr_t)(page_size - 1)),
                page_size, PROT_READ | PROT_WRITE) != 0)
      _exit(5);

   __atomic_store_n(&slot_inhandler[slot], 0, __ATOMIC_RELEASE);
}

static void *fault_worker(void *p)
{
   struct thread_ctx *ctx = (struct thread_ctx*)p;
   long   n;
   size_t pg;

   tls_ctx = ctx;
   if (slot_register() < 0)
   {
      fprintf(stderr, "NO SLOT for thread %d\n", ctx->id);
      exit(2);
   }

   for (n = 0; n < ctx->faults; n++)
   {
      pg = (size_t)(n % PAGES_PER_THREAD);
      /* Re-arm then touch: each touch is a real page fault. */
      if (mprotect(ctx->region + pg * page_size, page_size, PROT_NONE) != 0)
         exit(6);
      ctx->region[pg * page_size] = (uint8_t)n; /* faults here */
      ctx->local_count++;
   }

   slot_unregister();
   return NULL;
}

/* Races register/unregister against the lookup in live handlers. */
static volatile int churn_stop;

static void *churn_worker(void *p)
{
   (void)p;
   while (!__atomic_load_n(&churn_stop, __ATOMIC_ACQUIRE))
   {
      int s = slot_register();
      if (s >= 0)
         slot_unregister();
      sched_yield();
   }
   return NULL;
}

int main(int argc, char **argv)
{
   struct sigaction  sa;
   pthread_t         th[MAX_THREADS], churn;
   struct thread_ctx ctx[MAX_THREADS];
   int               n_threads = 4;
   long              faults    = 50000;
   int               i;
   uint64_t          expected  = 0;

   if (argc > 1) n_threads = atoi(argv[1]);
   if (argc > 2) faults    = atol(argv[2]);
   if (n_threads < 1 || n_threads > FAULT_SLOTS)
   {
      fprintf(stderr, "thread count must be 1..%d (slot table size)\n", FAULT_SLOTS);
      return 2;
   }

   page_size = (size_t)sysconf(_SC_PAGESIZE);

   memset(&sa, 0, sizeof(sa));
   sigemptyset(&sa.sa_mask);
   sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
   sa.sa_sigaction = fault_handler;
   if (sigaction(SIGSEGV, &sa, NULL) != 0)
      return 1;

   printf("fault dispatch stress: %d threads x %ld faults, %d slots\n",
          n_threads, faults, FAULT_SLOTS);

   for (i = 0; i < n_threads; i++)
   {
      ctx[i].id     = i;
      ctx[i].faults = faults;
      ctx[i].local_count = 0;
      ctx[i].region = (uint8_t*)mmap(NULL, PAGES_PER_THREAD * page_size,
                                     PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (ctx[i].region == MAP_FAILED)
         return 1;
   }

   /* Churn only makes sense with a free slot to contend for. */
   churn_stop = 0;
   if (n_threads < FAULT_SLOTS)
      pthread_create(&churn, NULL, churn_worker, NULL);

   for (i = 0; i < n_threads; i++)
      pthread_create(&th[i], NULL, fault_worker, &ctx[i]);
   for (i = 0; i < n_threads; i++)
      pthread_join(th[i], NULL);

   __atomic_store_n(&churn_stop, 1, __ATOMIC_RELEASE);
   if (n_threads < FAULT_SLOTS)
      pthread_join(churn, NULL);

   for (i = 0; i < n_threads; i++)
      expected += ctx[i].local_count;

   printf("  faults taken:     %llu\n", (unsigned long long)expected);
   printf("  guarded counter:  %llu\n", (unsigned long long)guarded_counter);

   if (saw_no_slot)
   {
      printf("FAIL: SLOT LOST - a fault arrived on a thread with no slot\n");
      return 3;
   }
   if (saw_recursion_conflict)
   {
      printf("FAIL: RECURSION FLAG - per-slot guard was not exclusive\n");
      return 4;
   }
   if (guarded_counter != expected)
   {
      printf("FAIL: COUNTER MISMATCH - spinlock did not exclude "
             "(%llu != %llu, lost %lld)\n",
             (unsigned long long)guarded_counter,
             (unsigned long long)expected,
             (long long)expected - (long long)guarded_counter);
      return 5;
   }

   printf("PASS: slot table, recursion guards and spinlock held under "
          "%d-way concurrent faulting.\n", n_threads);
   return 0;
}
