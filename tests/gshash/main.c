/* GS/Renderers/HW/GSHashTable.c: the open-addressed table meant to
 * replace the texture cache's unordered_maps.
 *
 * What is checked is what the texture cache needs of it: a value put in
 * comes back out, two keys that collide both survive and are told apart
 * by the caller's own comparison, a compact keeps what it is told to and
 * drops the rest, and the table still works afterwards. Also the part
 * that is easy to get wrong in an open-addressed table: a probe that
 * wraps the end of the array, and a table that is nearly full. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GSHashTable.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* A value: the caller's real key lives in it, as a palette's CLUT does. */
typedef struct entry
{
   unsigned key;
   unsigned payload;
} entry_t;

static entry_t *entries;

/* A spread-out hash, without a C99 long long constant. */
static uint64_t hash_of(unsigned i)
{
   return (uint64_t)i * 2654435761u + 0x9e3779b9u;
}

static entry_t *find(gs_hash_table_t *t, uint64_t hash, unsigned key)
{
   uint64_t h    = gs_hash_table_key(hash);
   unsigned slot = gs_hash_table_first(t, h);

   while (gs_hash_table_occupied(t, slot))
   {
      entry_t *e = (entry_t*)t->values[slot];
      if (t->hashes[slot] == h && e->key == key)
         return e;
      slot = gs_hash_table_next(t, slot);
   }
   return NULL;
}

static void insert(gs_hash_table_t *t, uint64_t hash, entry_t *e)
{
   uint64_t h    = gs_hash_table_key(hash);
   unsigned slot = gs_hash_table_first(t, h);

   while (gs_hash_table_occupied(t, slot))
      slot = gs_hash_table_next(t, slot);
   gs_hash_table_put(t, slot, h, e);
}

static int keep_even(void *value, void *ctx)
{
   (void)ctx;
   return (((entry_t*)value)->key & 1) == 0;
}

static int keep_none(void *value, void *ctx)
{
   (void)value; (void)ctx;
   return 0;
}

int main(void)
{
   gs_hash_table_t t;
   unsigned i;
   unsigned n = 400;

   memset(&t, 0, sizeof(t));
   entries = (entry_t*)calloc(n, sizeof(entry_t));

   CHECK(gs_hash_table_init(&t, 1024) != 0, "init");
   CHECK(t.capacity == 1024, "capacity is the power of two asked for");
   CHECK(gs_hash_table_init(&t, 1024) != 0, "init again is a no-op");

   /* A hash of zero is stored as one, because zero marks an empty slot. */
   CHECK(gs_hash_table_key(0) != 0, "a zero hash is not an empty slot");

   for (i = 0; i < n; i++)
   {
      entries[i].key     = i;
      entries[i].payload = i * 7 + 1;
      insert(&t, hash_of(i), &entries[i]);
   }
   CHECK(t.count == n, "everything went in");

   for (i = 0; i < n; i++)
   {
      entry_t *e = find(&t, hash_of(i), i);
      CHECK(e != NULL && e->payload == i * 7 + 1, "everything comes back out");
   }
   CHECK(find(&t, 12345, 999999) == NULL, "something absent is absent");

   /* Two entries under one hash: both live, and the caller's comparison
    * is what tells them apart. This is a palette hash collision. */
   {
      static entry_t a, b;
      a.key = 1000; a.payload = 11;
      b.key = 1001; b.payload = 22;
      insert(&t, 4242, &a);
      insert(&t, 4242, &b);
      CHECK(find(&t, 4242, 1000) == &a, "first of a collision found");
      CHECK(find(&t, 4242, 1001) == &b, "second of a collision found");
   }

   /* A probe that wraps: put several entries on the last slot. */
   {
      static entry_t w[4];
      uint64_t last = (uint64_t)(t.capacity - 1);
      for (i = 0; i < 4; i++)
      {
         w[i].key = 2000 + i; w[i].payload = i;
         insert(&t, last, &w[i]);
      }
      for (i = 0; i < 4; i++)
         CHECK(find(&t, last, 2000 + i) == &w[i], "a probe wraps the end");
   }

   gs_hash_table_compact(&t, keep_even, NULL);
   for (i = 0; i < n; i++)
   {
      entry_t *e = find(&t, hash_of(i), i);
      if (i & 1)
         CHECK(e == NULL, "compact dropped the odd ones");
      else
         CHECK(e != NULL && e->payload == i * 7 + 1, "compact kept the even ones");
   }
   /* n/2 of the numbered entries, plus the even-keyed ones among the six
    * added for the collision and wrap checks: 1000, 2000 and 2002. */
   CHECK(t.count == n / 2 + 3, "count after compact");

   /* Still usable: put the odd ones back. */
   for (i = 1; i < n; i += 2)
      insert(&t, hash_of(i), &entries[i]);
   for (i = 0; i < n; i++)
      CHECK(find(&t, hash_of(i), i) == &entries[i],
            "usable after compact");

   gs_hash_table_compact(&t, keep_none, NULL);
   CHECK(t.count == 0, "compact can empty it");
   CHECK(find(&t, 0, 0) == NULL, "and it is empty");

   gs_hash_table_clear(&t);
   CHECK(t.count == 0, "clear on an empty table");

   /* Nearly full: capacity minus one, which is the worst probe a correct
    * table has to survive. */
   {
      gs_hash_table_t small;
      entry_t *e = (entry_t*)calloc(15, sizeof(entry_t));
      memset(&small, 0, sizeof(small));
      CHECK(gs_hash_table_init(&small, 16) != 0, "small init");
      for (i = 0; i < 15; i++)
      {
         e[i].key = i; e[i].payload = i;
         insert(&small, i, &e[i]);
      }
      for (i = 0; i < 15; i++)
         CHECK(find(&small, i, i) == &e[i], "found in a nearly full table");
      gs_hash_table_free(&small);
      free(e);
   }

   gs_hash_table_free(&t);
   CHECK(t.hashes == NULL && t.capacity == 0, "free resets it");
   free(entries);

   printf(fails ? "gshash: FAILED (%d)\n" : "gshash: ok\n", fails);
   return fails != 0;
}
