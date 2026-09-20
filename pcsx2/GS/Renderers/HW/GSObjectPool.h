/*
 * A fixed pool of equally sized slots, in C.
 *
 * The texture cache makes and destroys sources and targets inside a
 * draw. Each one through the allocator is a cost that varies frame to
 * frame, and it is the reason a heap block header sits next to every
 * one of them. A pool is one allocation at startup and an index off a
 * free list per take and give: no allocator, no lock, no variation.
 *
 * Not thread safe and not meant to be: the GS runs on one thread.
 *
 * Past the pool's size, gs_object_pool_take returns NULL and the caller
 * falls back to the allocator, so a pool size is a budget rather than a
 * limit.
 */

#ifndef GS_OBJECT_POOL_H
#define GS_OBJECT_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slots are aligned to this, and so is anything the caller falls back to
 * the allocator for. Sixty-four because the GS compares and copies with
 * aligned SSE loads - GSVector4i::compare64 and friends cast a pointer
 * and index it - and the buffers those run over live inside pooled
 * objects. Sixteen would do for SSE; sixty-four is the cache line and is
 * what the allocations these pools replaced asked for. */
#define GS_OBJECT_POOL_ALIGN 64

typedef struct gs_object_pool
{
   unsigned char  *slots;        /* one allocation, count * stride     */
   unsigned short *free_indices; /* the slots nobody holds             */
   size_t          stride;       /* bytes per slot, 32-byte aligned    */
   unsigned        count;        /* how many slots there are           */
   unsigned        free_count;   /* how many of them are free          */
} gs_object_pool_t;

/* Sizes the pool for objects of `size` bytes and allocates it. Returns 0
 * if the allocation failed, in which case take always returns NULL and
 * the caller uses the allocator for everything. Calling it again on a
 * pool that is already set up does nothing. Cold: once per pool. */
int  gs_object_pool_init(gs_object_pool_t *pool, size_t size, unsigned count);

void gs_object_pool_free(gs_object_pool_t *pool);

/* Take and give are here rather than in the .c on purpose: they are what
 * a draw pays, they are a handful of instructions each, and out of line
 * they would be a call the C++ they replaced did not make. Static in the
 * header is what C89 has instead of inline; the generated code for both
 * is the same as the version this replaced (tests/gspool checks the
 * behaviour, and the commit that added them compares the assembly). */

/* A slot, or NULL when the pool is empty or `size` does not fit one. */
static void *gs_object_pool_take(gs_object_pool_t *pool, size_t size)
{
   if (!pool->free_count || size > pool->stride)
      return NULL;
   return pool->slots + (size_t)pool->free_indices[--pool->free_count] * pool->stride;
}

/* Returns a slot to the pool. Tells the caller whether `p` was one:
 * nonzero if it was and has been taken back, zero if it came from
 * somewhere else and is still theirs to free. */
static int gs_object_pool_give(gs_object_pool_t *pool, void *p)
{
   size_t off;

   if (!pool->slots)
      return 0;
   if ((unsigned char*)p < pool->slots)
      return 0;
   off = (size_t)((unsigned char*)p - pool->slots);
   if (off >= pool->stride * pool->count)
      return 0;

   if (pool->free_count < pool->count)
      pool->free_indices[pool->free_count++] = (unsigned short)(off / pool->stride);
   return 1;
}

#ifdef __cplusplus
}
#endif

#endif
