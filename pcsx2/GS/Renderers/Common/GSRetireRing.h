/* Textures the frontend may still be showing, kept until it cannot be:
 * a present texture that has been replaced, resized or dropped for a
 * state load. C89.
 *
 * Each is stamped with the present count and the frontend's sync-wait
 * count when retired. It is done once the frontend has waited through
 * every sync slot since (where it says how many it has), or else once a
 * minimum number of presents has gone by. Nothing is freed before then:
 * the list grows instead, however many are retired between presents - a
 * state load retires several at once, and rollback loads one every
 * frame. */
#ifndef GS_RETIRE_RING_H
#define GS_RETIRE_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_retired
{
	void*    tex;
	uint32_t presents;
	uint32_t waits;
};

struct gs_retire_ring
{
	struct gs_retired* list;
	uint32_t           count;
	uint32_t           cap;
};

typedef void (*gs_retire_free_fn)(void* tex);

/* Keeps tex until it is done. Only if the list cannot grow is the
 * oldest entry freed at once to make room. */
void gs_retire_add(struct gs_retire_ring* r, void* tex, uint32_t presents,
	uint32_t waits, gs_retire_free_fn free_fn);

/* Frees every entry that is done: with sync_slots nonzero, once waits
 * has moved more than sync_slots past its stamp; otherwise once presents
 * has moved more than min_presents past. */
void gs_retire_age(struct gs_retire_ring* r, uint32_t presents, uint32_t waits,
	uint32_t sync_slots, uint32_t min_presents, gs_retire_free_fn free_fn);

/* Frees every entry and the list: nothing presents any more. */
void gs_retire_clear(struct gs_retire_ring* r, gs_retire_free_fn free_fn);

#ifdef __cplusplus
}
#endif

#endif
