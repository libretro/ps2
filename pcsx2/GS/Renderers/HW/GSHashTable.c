/* The header's probe helpers are static, so a translation unit using
 * only the cold half - this one - would warn about them. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include <stdlib.h>
#include <string.h>

#include "GSHashTable.h"

int gs_hash_table_init(gs_hash_table_t *t, unsigned capacity)
{
   unsigned cap = 8;

   if (t->hashes)
      return 1;

   while (cap < capacity)
      cap <<= 1;

   t->hashes = (uint64_t*)calloc(cap, sizeof(uint64_t));
   if (!t->hashes)
      return 0;
   t->values = (void**)calloc(cap, sizeof(void*));
   if (!t->values)
   {
      free(t->hashes);
      t->hashes = NULL;
      return 0;
   }

   t->capacity = cap;
   t->mask     = cap - 1;
   t->count    = 0;
   return 1;
}

void gs_hash_table_free(gs_hash_table_t *t)
{
   if (t->hashes)
      free(t->hashes);
   if (t->values)
      free(t->values);
   t->hashes   = NULL;
   t->values   = NULL;
   t->capacity = 0;
   t->mask     = 0;
   t->count    = 0;
}

void gs_hash_table_clear(gs_hash_table_t *t)
{
   if (!t->hashes)
      return;
   memset(t->hashes, 0, t->capacity * sizeof(uint64_t));
   t->count = 0;
}

void gs_hash_table_compact(gs_hash_table_t *t,
      int (*keep)(void *value, void *ctx), void *ctx)
{
   uint64_t *old_hashes;
   void    **old_values;
   unsigned  i;

   if (!t->hashes || !t->count)
      return;

   /* Rebuilt rather than patched: removing from an open-addressed table
    * in place either leaves tombstones, which every later probe pays
    * for, or shifts runs around, which is easy to get wrong. This is the
    * cold path. */
   old_hashes = (uint64_t*)malloc(t->capacity * sizeof(uint64_t));
   if (!old_hashes)
      return;
   old_values = (void**)malloc(t->capacity * sizeof(void*));
   if (!old_values)
   {
      free(old_hashes);
      return;
   }

   memcpy(old_hashes, t->hashes, t->capacity * sizeof(uint64_t));
   memcpy(old_values, t->values, t->capacity * sizeof(void*));

   memset(t->hashes, 0, t->capacity * sizeof(uint64_t));
   t->count = 0;

   for (i = 0; i < t->capacity; i++)
   {
      unsigned slot;

      if (!old_hashes[i])
         continue;
      if (!keep(old_values[i], ctx))
         continue;

      slot = (unsigned)old_hashes[i] & t->mask;
      while (t->hashes[slot])
         slot = (slot + 1) & t->mask;

      t->hashes[slot] = old_hashes[i];
      t->values[slot] = old_values[i];
      t->count++;
   }

   free(old_hashes);
   free(old_values);
}
