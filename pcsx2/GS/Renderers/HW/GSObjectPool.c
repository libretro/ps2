#include <stdlib.h>
#include <memalign.h>

/* The header's take and give are static, so a translation unit that uses
 * neither - this one, which only sets the pool up - would warn about
 * them. Say they are meant to be there. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSObjectPool.h"

int gs_object_pool_init(gs_object_pool_t *pool, size_t size, unsigned count)
{
   unsigned i;
   size_t   stride;

   if (pool->slots)
      return 1;
   if (!count)
      return 0;

   /* Slots keep the alignment the objects ask for, so the address of
    * every one of them is as good as the allocator's. */
   stride = (size + (GS_OBJECT_POOL_ALIGN - 1)) & ~((size_t)GS_OBJECT_POOL_ALIGN - 1);

   pool->slots = (unsigned char*)memalign_alloc(GS_OBJECT_POOL_ALIGN, stride * count);
   if (!pool->slots)
      return 0;

   pool->free_indices = (unsigned short*)malloc(count * sizeof(unsigned short));
   if (!pool->free_indices)
   {
      memalign_free(pool->slots);
      pool->slots = NULL;
      return 0;
   }

   /* Zeroed: nothing is out with a caller yet. */
   pool->in_use = (unsigned char*)calloc(count, 1);
   if (!pool->in_use)
   {
      free(pool->free_indices);
      pool->free_indices = NULL;
      memalign_free(pool->slots);
      pool->slots = NULL;
      return 0;
   }
   pool->double_frees = 0;

   /* Handed out from the end, so a run of takes gets the low slots and
    * they stay near each other. */
   for (i = 0; i < count; i++)
      pool->free_indices[i] = (unsigned short)(count - 1 - i);

   pool->stride     = stride;
   pool->count      = count;
   pool->free_count = count;
   return 1;
}

void gs_object_pool_free(gs_object_pool_t *pool)
{
   if (pool->slots)
      memalign_free(pool->slots);
   if (pool->free_indices)
      free(pool->free_indices);
   if (pool->in_use)
      free(pool->in_use);
   pool->slots        = NULL;
   pool->free_indices = NULL;
   pool->in_use       = NULL;
   pool->stride       = 0;
   pool->count        = 0;
   pool->free_count   = 0;
}
