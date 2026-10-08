/* Every texture the frontend may still be showing is alive, on the real
 * retire list (GSRetireRing.c) as GSDevice drives it.
 *
 * The hardware renderer hands the frontend its merge target every VSync,
 * and the frontend keeps sampling what it was handed until it has waited
 * through its sync slots, and replays the last one while the core is
 * paused. A state load or a reset retires the present textures - merge,
 * weavebob, blend and mad - and play goes on with new ones. So:
 *   - pause, load, the frontend replays: the texture it shows is alive;
 *   - rollback, a load before every frame, four textures retired each
 *     time: every texture handed over within the last sync slots is
 *     alive, however many have been retired since;
 *   - with no sync slots known, the present-count rule holds instead;
 *   - everything retired is freed in the end, and clearing frees the
 *     rest.
 * A fixed ring of eight that frees the slot it comes back round to is
 * run through the rollback case as the control, and must lose a
 * texture the frontend is still showing. C89.
 */
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSRetireRing.h"

#define SYNC_SLOTS 3   /* what the frontend usually offers */
#define MIN_PRESENTS 8 /* GSDevice::RETIRED_PRESENT_MIN_AGE */
#define MAX_TEX 4096

static int s_alive[MAX_TEX];
static int s_next;

static void* tex_new(void)
{
	s_alive[s_next] = 1;
	return &s_alive[s_next++];
}

static void tex_free(void* t)
{
	*(int*)t = 0;
}

static int s_failures;

/* What the frontend was handed at each wait. */
static void* s_handed[MAX_TEX];

/* The control: eight slots, the one come round to freed regardless. */
struct fixed_ring { void* tex[8]; unsigned slot; };
static void fixed_add(struct fixed_ring* r, void* t)
{
	if (r->tex[r->slot])
		tex_free(r->tex[r->slot]);
	r->tex[r->slot] = t;
	r->slot = (r->slot + 1) % 8;
}

/* Rollback: each frame presents a fresh merge target, waits, and is then
 * undone by a load that retires the four present textures. Returns how
 * many times the frontend could have been showing a freed texture. */
static int rollback(int control, int frames, int* leaked)
{
	struct gs_retire_ring r;
	struct fixed_ring f;
	unsigned waits = 0, presents = 0;
	int frame, k, lost = 0;

	memset(&r, 0, sizeof(r));
	memset(&f, 0, sizeof(f));
	memset(s_alive, 0, sizeof(s_alive));
	s_next = 0;
	for (frame = 0; frame < frames; frame++)
	{
		void* present[4];
		for (k = 0; k < 4; k++)
			present[k] = tex_new();
		/* VSync: wait for a slot, hand over the merge target, age. */
		waits++;
		presents++;
		s_handed[waits] = present[0];
		if (!control)
			gs_retire_age(&r, presents, waits, SYNC_SLOTS, MIN_PRESENTS, tex_free);
		/* The load: all four retired. */
		for (k = 0; k < 4; k++)
		{
			if (control)
				fixed_add(&f, present[k]);
			else
				gs_retire_add(&r, present[k], presents, waits, tex_free);
		}
		/* The frontend may still be sampling anything handed over in its
		 * last SYNC_SLOTS waits. */
		for (k = 0; k <= SYNC_SLOTS && (unsigned)k < waits; k++)
			if (!*(int*)s_handed[waits - k])
				lost++;
	}
	*leaked = 0;
	if (!control)
	{
		/* Play on without loads: the frontend moves past them all. */
		for (k = 0; k < SYNC_SLOTS + 2; k++)
		{
			waits++;
			presents++;
			gs_retire_age(&r, presents, waits, SYNC_SLOTS, MIN_PRESENTS, tex_free);
		}
		*leaked = (int)r.count;
		gs_retire_clear(&r, tex_free);
	}
	return lost;
}

int main(void)
{
	struct gs_retire_ring r;
	int leaked = 0, lost, k;
	unsigned presents = 0, waits = 0;
	void* shown;

	/* Pause, load, replay. */
	memset(&r, 0, sizeof(r));
	shown = tex_new();
	gs_retire_add(&r, shown, presents, waits, tex_free);
	if (!*(int*)shown)
	{
		printf("  FAIL: the frame the frontend replays after a load is freed\n");
		s_failures++;
	}
	for (k = 0; k < SYNC_SLOTS + 2; k++)
		gs_retire_age(&r, ++presents, ++waits, SYNC_SLOTS, MIN_PRESENTS, tex_free);
	if (*(int*)shown || r.count)
	{
		printf("  FAIL: the replayed frame is never freed\n");
		s_failures++;
	}

	/* No sync slots known: held for MIN_PRESENTS presents. */
	shown = tex_new();
	gs_retire_add(&r, shown, presents, waits, tex_free);
	for (k = 0; k < MIN_PRESENTS; k++)
		gs_retire_age(&r, ++presents, waits, 0, MIN_PRESENTS, tex_free);
	if (!*(int*)shown)
	{
		printf("  FAIL: freed before %d presents with no sync slots known\n", MIN_PRESENTS);
		s_failures++;
	}
	gs_retire_age(&r, ++presents, waits, 0, MIN_PRESENTS, tex_free);
	if (*(int*)shown)
	{
		printf("  FAIL: still held past %d presents\n", MIN_PRESENTS);
		s_failures++;
	}

	/* Clearing frees what is held. */
	shown = tex_new();
	gs_retire_add(&r, shown, presents, waits, tex_free);
	gs_retire_clear(&r, tex_free);
	if (*(int*)shown || r.list || r.count)
	{
		printf("  FAIL: clearing leaves a texture or the list\n");
		s_failures++;
	}

	/* Rollback, a load every frame. */
	lost = rollback(0, 200, &leaked);
	if (lost)
	{
		printf("  FAIL: under rollback the frontend lost %d textures it may still show\n", lost);
		s_failures++;
	}
	if (leaked)
	{
		printf("  FAIL: %d retired textures never freed\n", leaked);
		s_failures++;
	}
	printf("  rollback, 200 loads of four textures: %d lost, %d left over\n", lost, leaked);
	lost = rollback(1, 200, &leaked);
	printf("  control, a fixed ring of eight: %d lost\n", lost);
	if (!lost)
	{
		printf("  FAIL: the control loses nothing; the check sees no early free\n");
		s_failures++;
	}

	printf(s_failures ? "present lifetime: FAILED (%d)\n" : "present lifetime: ok\n", s_failures);
	return s_failures != 0;
}
