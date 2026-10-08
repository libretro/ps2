/* See GSRetireRing.h. C89. */
#include <stdlib.h>
#include <string.h>

#include "GSRetireRing.h"

void gs_retire_add(struct gs_retire_ring* r, void* tex, uint32_t presents,
	uint32_t waits, gs_retire_free_fn free_fn)
{
	if (!tex)
		return;
	if (r->count == r->cap)
	{
		const uint32_t cap = r->cap ? r->cap * 2 : 8;
		struct gs_retired* grown = (struct gs_retired*)realloc(r->list, cap * sizeof(*grown));
		if (grown)
		{
			r->list = grown;
			r->cap  = cap;
		}
		else if (r->count)
		{
			/* No room to keep one more: the oldest goes now. */
			free_fn(r->list[0].tex);
			memmove(r->list, r->list + 1, (r->count - 1) * sizeof(*r->list));
			r->count--;
		}
		else
		{
			free_fn(tex);
			return;
		}
	}
	r->list[r->count].tex      = tex;
	r->list[r->count].presents = presents;
	r->list[r->count].waits    = waits;
	r->count++;
}

void gs_retire_age(struct gs_retire_ring* r, uint32_t presents, uint32_t waits,
	uint32_t sync_slots, uint32_t min_presents, gs_retire_free_fn free_fn)
{
	uint32_t i, kept = 0;
	for (i = 0; i < r->count; i++)
	{
		const struct gs_retired* e = &r->list[i];
		const int done = sync_slots
			? (uint32_t)(waits - e->waits) > sync_slots
			: (uint32_t)(presents - e->presents) > min_presents;
		if (done)
			free_fn(e->tex);
		else
			r->list[kept++] = *e;
	}
	r->count = kept;
}

void gs_retire_clear(struct gs_retire_ring* r, gs_retire_free_fn free_fn)
{
	uint32_t i;
	for (i = 0; i < r->count; i++)
		free_fn(r->list[i].tex);
	free(r->list);
	r->list  = NULL;
	r->count = 0;
	r->cap   = 0;
}
