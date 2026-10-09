/* paraLLEl-GS's command-pool thread indices, on the real thread_id.cpp.
 *
 * The device keeps one command pool per thread index, and a thread's index
 * is handed out on its first request. Each device - one per session, and
 * a core image can run many sessions - has pools of its own, so:
 *   - within a device, the device thread holds 0 and every other recording
 *     thread a distinct index of its own, never 0;
 *   - a later device hands its indices out again from 1, to new threads and
 *     to threads kept from the session before alike, so session after
 *     session no thread is ever pushed onto pool 0.
 * C89. */
#include <stdio.h>

#include <rthreads/rthreads.h>

#define COUNT    4 /* GSRendererPGS's PGS_THREAD_INDEX_COUNT */
#define SESSIONS 6

void     tid_set_count(unsigned count);
void     tid_register(unsigned index);
unsigned tid_get(void);

static unsigned s_got[COUNT];

static void ask(void *slot)
{
	*(unsigned*)slot = tid_get();
}

/* A thread kept across sessions: asks once per session when told to. */
static slock_t  *s_lock;
static scond_t  *s_cond;
static int       s_round, s_answered, s_quit;
static unsigned  s_kept_index;

static void kept_thread(void *unused)
{
	int seen = 0;
	(void)unused;
	slock_lock(s_lock);
	for (;;)
	{
		while (s_round == seen && !s_quit)
			scond_wait(s_cond, s_lock);
		if (s_quit)
			break;
		seen = s_round;
		slock_unlock(s_lock);
		s_kept_index = tid_get();
		slock_lock(s_lock);
		s_answered = seen;
		scond_broadcast(s_cond);
	}
	slock_unlock(s_lock);
}

int main(void)
{
	sthread_t *kept, *t[COUNT];
	int session, i, j, failures = 0;

	s_lock = slock_new();
	s_cond = scond_new();
	kept   = sthread_create(kept_thread, NULL);

	for (session = 1; session <= SESSIONS; session++)
	{
		unsigned seen[COUNT + 1];
		int n = 0;

		/* The device is made: the count, then its own thread takes 0. */
		tid_set_count(COUNT);
		tid_register(0);
		seen[n++] = tid_get();

		/* The thread kept from the last session records again. */
		slock_lock(s_lock);
		s_round = session;
		scond_broadcast(s_cond);
		while (s_answered != session)
			scond_wait(s_cond, s_lock);
		slock_unlock(s_lock);
		seen[n++] = s_kept_index;

		/* And new threads, up to the pools there are. */
		for (i = 0; i < COUNT - 2; i++)
			t[i] = sthread_create(ask, &s_got[i]);
		for (i = 0; i < COUNT - 2; i++)
		{
			sthread_join(t[i]);
			seen[n++] = s_got[i];
		}

		if (seen[0] != 0)
		{
			printf("  FAIL: session %d: the device thread holds %u, not 0\n", session, seen[0]);
			failures++;
		}
		for (i = 1; i < n; i++)
		{
			if (seen[i] == 0 || seen[i] >= COUNT)
			{
				printf("  FAIL: session %d: a recording thread was given index %u\n", session, seen[i]);
				failures++;
			}
			for (j = 0; j < i; j++)
				if (seen[i] == seen[j])
				{
					printf("  FAIL: session %d: two threads share index %u\n", session, seen[i]);
					failures++;
				}
		}
	}

	slock_lock(s_lock);
	s_quit = 1;
	scond_broadcast(s_cond);
	slock_unlock(s_lock);
	sthread_join(kept);
	scond_free(s_cond);
	slock_free(s_lock);

	printf(failures ? "thread indices: FAILED (%d)\n" : "thread indices: ok, %d sessions\n",
			failures ? failures : SESSIONS);
	return failures != 0;
}
