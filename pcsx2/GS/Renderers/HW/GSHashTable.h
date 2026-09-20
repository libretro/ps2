/*
 * An open-addressed table of 64-bit hash to pointer, in C.
 *
 * What it replaces is std::unordered_map: a bucket array of pointers to
 * separately allocated nodes, so every lookup is a pointer chase into a
 * node that shares a cache line with nothing else, and every insertion
 * is an allocation.
 *
 * Here the hashes are one flat array and the values another. A lookup
 * reads consecutive hashes until it finds the one it wants or an empty
 * slot, which is a handful of u64 loads out of the same cache line, and
 * an insertion writes two words. The table is sized once and never
 * grows, so nothing allocates after that.
 *
 * Capacity must be a power of two and should be at least twice the
 * number of entries meant to live in it: probes lengthen sharply past a
 * load factor of about a half.
 *
 * A hash of zero marks an empty slot, so a caller's zero is stored as
 * one. Two entries whose hashes collide both live in the table; the
 * caller confirms a candidate with whatever its real key comparison is,
 * which is why the probe below hands out candidates rather than one
 * answer.
 *
 * Removal is bulk only, through compact: the palette maps and the hash
 * cache drop everything unused in one pass, and a table with no
 * tombstones is simpler and faster to probe.
 *
 * Not thread safe; the GS is one thread.
 */

#ifndef GS_HASH_TABLE_H
#define GS_HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_hash_table
{
   uint64_t *hashes;   /* 0 is an empty slot                      */
   void    **values;
   unsigned  capacity; /* a power of two                          */
   unsigned  mask;     /* capacity - 1                            */
   unsigned  count;
} gs_hash_table_t;

/* Returns 0 if the allocation failed. capacity is rounded up to a power
 * of two. Cold: once per table. */
int  gs_hash_table_init(gs_hash_table_t *t, unsigned capacity);
void gs_hash_table_free(gs_hash_table_t *t);
void gs_hash_table_clear(gs_hash_table_t *t);

/* Keeps the entries `keep` returns nonzero for and drops the rest,
 * rebuilding the table so no tombstones are left behind. Cold. */
void gs_hash_table_compact(gs_hash_table_t *t,
      int (*keep)(void *value, void *ctx), void *ctx);

/* --- the hot part, here so it inlines ---------------------------------
 *
 *   uint64_t h    = gs_hash_table_key(my_hash);
 *   unsigned slot = gs_hash_table_first(t, h);
 *   while (gs_hash_table_occupied(t, slot))
 *   {
 *      if (t->hashes[slot] == h && my_key_matches(t->values[slot]))
 *         return t->values[slot];
 *      slot = gs_hash_table_next(t, slot);
 *   }
 *   -- slot is now where an insertion goes
 */

static uint64_t gs_hash_table_key(uint64_t hash)
{
   return hash ? hash : 1;
}

static unsigned gs_hash_table_first(const gs_hash_table_t *t, uint64_t h)
{
   return (unsigned)(h)&t->mask;
}

static unsigned gs_hash_table_next(const gs_hash_table_t *t, unsigned slot)
{
   return (slot + 1) & t->mask;
}

static int gs_hash_table_occupied(const gs_hash_table_t *t, unsigned slot)
{
   return t->hashes[slot] != 0;
}

/* Puts a value in a slot a probe stopped at. The caller must have probed
 * to an empty slot, and the table must have room - keep it under half
 * full and that is the same thing. */
static void gs_hash_table_put(gs_hash_table_t *t, unsigned slot,
      uint64_t h, void *value)
{
   t->hashes[slot] = h;
   t->values[slot] = value;
   t->count++;
}

#ifdef __cplusplus
}
#endif

#endif
