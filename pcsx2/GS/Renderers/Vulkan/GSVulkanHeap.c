#include <stdlib.h>
#include <string.h>

#include "GSVulkanHeap.h"

#define GS_VK_HEAP_MIN_SPANS 16

static VkDeviceSize gs_vk_align_up(VkDeviceSize v, VkDeviceSize a)
{
   if (a <= 1)
      return v;
   return (v + (a - 1)) & ~(a - 1);
}

/* The memory type this can live in. preferred is tried first so that a
 * device-local host-visible type is taken where one exists, and dropped
 * when none does. */
static int gs_vk_pick_type(const gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      unsigned *out)
{
   unsigned pass;

   for (pass = 0; pass < 2; pass++)
   {
      const VkMemoryPropertyFlags want = (pass == 0)
         ? (required | preferred) : required;
      unsigned i;

      for (i = 0; i < heap->props.memoryTypeCount; i++)
      {
         if (!(type_bits & (1u << i)))
            continue;
         if ((heap->props.memoryTypes[i].propertyFlags & want) != want)
            continue;
         *out = i;
         return 1;
      }

      if (!preferred)
         break;
   }
   return 0;
}

/* The free spans of a block are kept sorted by offset and never adjacent:
 * a free then finds its place with a binary search and merges with the
 * span on either side, where it used to compare every span with every
 * other. Makes room for one more span at index at. */
static int gs_vk_block_insert_span(gs_vk_block_t *b, unsigned at,
      VkDeviceSize offset, VkDeviceSize size)
{
   if (b->free_count == b->free_capacity)
   {
      const unsigned cap = b->free_capacity ? b->free_capacity * 2u : GS_VK_HEAP_MIN_SPANS;
      gs_vk_span_t *grown = (gs_vk_span_t*)realloc(b->free_spans, cap * sizeof(gs_vk_span_t));
      if (!grown)
         return 0;
      b->free_spans    = grown;
      b->free_capacity = cap;
   }
   if (at < b->free_count)
      memmove(&b->free_spans[at + 1], &b->free_spans[at],
            (b->free_count - at) * sizeof(gs_vk_span_t));
   b->free_spans[at].offset = offset;
   b->free_spans[at].size   = size;
   b->free_count++;
   return 1;
}

static void gs_vk_block_remove_span(gs_vk_block_t *b, unsigned at)
{
   b->free_count--;
   if (at < b->free_count)
      memmove(&b->free_spans[at], &b->free_spans[at + 1],
            (b->free_count - at) * sizeof(gs_vk_span_t));
}

/* One block from the driver, whole and mapped if it can be. */
static int gs_vk_heap_add_block(gs_vk_heap_t *heap, unsigned type, unsigned linear,
      VkDeviceSize size)
{
   VkMemoryAllocateInfo mai;
   gs_vk_block_t *b;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped = NULL;
   unsigned i_hole;

   unsigned slot = heap->block_count;

   /* A slot trim left empty, if there is one. */
   for (i_hole = 0; i_hole < heap->block_count; i_hole++)
   {
      if (!heap->blocks[i_hole].memory)
      {
         slot = i_hole;
         break;
      }
   }

   if (slot >= GS_VK_HEAP_MAX_BLOCKS)
      return 0;

   /* The ceiling. Without one the heap will hand out blocks until the
    * card is gone, which is what it did: sixty-four blocks of half a
    * gigabyte is thirty-two, and a 5090 has thirty-one and a half. A
    * heap that refuses is a renderer with a missing texture and a line
    * in the log; a heap that does not is a dead machine. */
   if (heap->max_bytes && heap->bytes_reserved + size > heap->max_bytes)
      return 0;

   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = size;
   mai.memoryTypeIndex = type;

   if (heap->fns.allocate_memory(heap->device, &mai, NULL, &memory) != VK_SUCCESS)
      return 0;

   if (heap->props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
   {
      /* Mapped once, for the life of the block: nothing maps per use. */
      if (heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
         mapped = NULL;
   }

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   b->memory   = memory;
   b->mapped   = mapped;
   b->size     = size;
   b->max_free = size;
   b->type     = type;
   b->linear   = linear;

   if (!gs_vk_block_insert_span(b, 0, 0, size))
   {
      if (mapped)
         heap->fns.unmap_memory(heap->device, memory);
      heap->fns.free_memory(heap->device, memory, NULL);
      return 0;
   }

   if (slot == heap->block_count)
      heap->block_count++;
   heap->bytes_reserved += size;
   return 1;
}

int gs_vk_heap_init(gs_vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const gs_vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize non_coherent_atom_size, VkDeviceSize max_bytes)
{
   if (!heap || !props || !fns || !fns->allocate_memory)
      return 0;

   memset(heap, 0, sizeof(*heap));
   heap->device     = device;
   heap->props      = *props;
   heap->fns        = *fns;
   heap->block_size = block_size ? block_size : (64u * 1024u * 1024u);
   heap->atom_size  = non_coherent_atom_size ? non_coherent_atom_size : 256u;
   heap->max_bytes  = max_bytes;
   return 1;
}

void gs_vk_heap_shutdown(gs_vk_heap_t *heap)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_vk_block_t *b = &heap->blocks[i];
      if (!b->memory)
         continue;
      if (b->mapped)
         heap->fns.unmap_memory(heap->device, b->memory);
      heap->fns.free_memory(heap->device, b->memory, NULL);
      if (b->free_spans)
         free(b->free_spans);
   }
   heap->block_count    = 0;
   heap->bytes_reserved = 0;
   heap->bytes_used     = 0;
   heap->bytes_host     = 0;
   heap->bytes_device   = 0;
}

unsigned gs_vk_heap_reserve(gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags flags, int linear, unsigned blocks)
{
   unsigned type = 0;
   unsigned made = 0;
   unsigned i;

   if (!gs_vk_pick_type(heap, type_bits, flags, 0, &type))
      return 0;

   for (i = 0; i < blocks; i++)
   {
      if (!gs_vk_heap_add_block(heap, type, linear ? 1u : 0u, heap->block_size))
         break;
      made++;
   }
   return made;
}

/* The lowest span of the block that fits, honouring the alignment the
 * requirement asks for. The leftovers on either side stay free. */
static int gs_vk_block_alloc(gs_vk_heap_t *heap, gs_vk_block_t *b, unsigned index,
      VkDeviceSize size, VkDeviceSize align, gs_vk_alloc_t *out)
{
   VkDeviceSize largest = 0;
   unsigned i;

   /* No span of this block is larger than max_free, so a block that
    * cannot hold this is passed over without its spans being read. */
   if (size > b->max_free)
      return 0;

   for (i = 0; i < b->free_count; i++)
   {
      const VkDeviceSize start     = b->free_spans[i].offset;
      const VkDeviceSize span_size = b->free_spans[i].size;
      const VkDeviceSize aligned   = gs_vk_align_up(start, align);
      const VkDeviceSize head      = aligned - start;
      VkDeviceSize tail;

      if (span_size > largest)
         largest = span_size;
      if (head + size > span_size)
         continue;

      tail = span_size - head - size;

      if (head && tail)
      {
         /* Split: the head stays where it is, the tail goes in after it. */
         if (!gs_vk_block_insert_span(b, i + 1, aligned + size, tail))
            return 0;
         b->free_spans[i].size = head;
      }
      else if (head)
         b->free_spans[i].size = head;
      else if (tail)
      {
         b->free_spans[i].offset = aligned + size;
         b->free_spans[i].size   = tail;
      }
      else
         gs_vk_block_remove_span(b, i);

      b->used          += size;
      heap->bytes_used += size;
      if (heap->props.memoryTypes[b->type].propertyFlags
            & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
         heap->bytes_host += size;
      else
         heap->bytes_device += size;

      out->memory = b->memory;
      out->offset = aligned;
      out->size   = size;
      out->mapped = b->mapped ? (void*)((unsigned char*)b->mapped + aligned) : NULL;
      out->block  = index;
      out->type   = b->type;
      return 1;
   }

   /* Every span was looked at and none fits: now the bound is exact. */
   b->max_free = largest;
   return 0;
}

unsigned gs_vk_heap_trim(gs_vk_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      gs_vk_block_t *b = &heap->blocks[i];

      if (!b->memory || b->used != 0)
         continue;

      if (b->mapped)
         heap->fns.unmap_memory(heap->device, b->memory);
      heap->fns.free_memory(heap->device, b->memory, NULL);
      if (b->free_spans)
         free(b->free_spans);

      heap->bytes_reserved -= b->size;
      freed++;

      /* A hole, not a gap closed up. Every live allocation carries the
       * index of its block, so the array must not be compacted - moving
       * the last block into this slot would leave those allocations
       * pointing at someone else's memory. The slot is reused by the
       * next block instead. */
      memset(b, 0, sizeof(*b));
   }
   return freed;
}

/* An allocation in one memory type: a block of that type with room, or
 * a new block of it. may_trim is clear while another type is still left
 * to try, so that a refused preference does not give back blocks the
 * frames ahead were reserved for. */
static int gs_vk_heap_alloc_in(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int may_trim, gs_vk_alloc_t *out)
{
   unsigned i;
   VkDeviceSize block_size;

   for (i = 0; i < heap->block_count; i++)
   {
      if (!heap->blocks[i].memory || heap->blocks[i].type != type
            || heap->blocks[i].linear != linear)
         continue;
      if (gs_vk_block_alloc(heap, &heap->blocks[i], i, req->size, req->alignment, out))
         return 1;
   }

   /* Nothing fits. A new block, which is the only thing here that asks
    * the driver for memory, and the reason gs_vk_heap_reserve exists:
    * reserve enough at startup and this never runs. */
   block_size = heap->block_size;
   if (block_size < req->size)
      block_size = gs_vk_align_up(req->size, 64u * 1024u);

   if (!gs_vk_heap_add_block(heap, type, linear, block_size))
   {
      /* No room for another block. Empty ones are given back first -
       * a run that filled the ceiling with upload blocks has nothing
       * for an image even with most of it free - and then one more
       * try. */
      if (!may_trim || !gs_vk_heap_trim(heap))
         return 0;
      if (!gs_vk_heap_add_block(heap, type, linear, block_size))
         return 0;
   }

   for (i = 0; i < heap->block_count; i++)
   {
      if (!heap->blocks[i].memory || heap->blocks[i].type != type
            || heap->blocks[i].linear != linear)
         continue;
      if (gs_vk_block_alloc(heap, &heap->blocks[i], i, req->size, req->alignment, out))
         return 1;
   }
   return 0;
}

int gs_vk_heap_alloc(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      int linear, gs_vk_alloc_t *out)
{
   const unsigned kind = linear ? 1u : 0u;
   unsigned type     = 0;
   unsigned fallback = 0;
   int has_fallback;

   if (!gs_vk_pick_type(heap, req->memoryTypeBits, required, preferred, &type))
      return 0;

   /* preferred is a preference. The device-local host-visible type is
    * the PCI BAR window on discrete cards without resizable BAR -
    * 256 MB or less, shared with the driver and the frontend - and the
    * driver refuses a block there long before the device runs out. The
    * type that only meets the requirement takes it instead. */
   has_fallback = preferred
      && gs_vk_pick_type(heap, req->memoryTypeBits, required, 0, &fallback)
      && fallback != type;

   if (gs_vk_heap_alloc_in(heap, req, type, kind, !has_fallback, out))
      return 1;
   return has_fallback && gs_vk_heap_alloc_in(heap, req, fallback, kind, 1, out);
}

void gs_vk_heap_free(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   gs_vk_block_t *b;
   VkDeviceSize offset;
   VkDeviceSize size;
   unsigned lo;
   unsigned hi;
   int joins_prev;
   int joins_next;

   if (!alloc || alloc->memory == VK_NULL_HANDLE || alloc->block >= heap->block_count)
      return;

   b = &heap->blocks[alloc->block];
   if (b->used >= alloc->size)
      b->used -= alloc->size;
   if (heap->bytes_used >= alloc->size)
      heap->bytes_used -= alloc->size;
   if (heap->props.memoryTypes[b->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
   {
      if (heap->bytes_host >= alloc->size)
         heap->bytes_host -= alloc->size;
   }
   else if (heap->bytes_device >= alloc->size)
      heap->bytes_device -= alloc->size;

   /* Put back where it belongs in the sorted spans and joined to
    * whichever neighbours it touches, so a block does not turn into a
    * thousand unusable slivers over a run. The first free span that
    * starts after this one: */
   offset = alloc->offset;
   size   = alloc->size;
   lo     = 0;
   hi     = b->free_count;
   while (lo < hi)
   {
      const unsigned mid = lo + (hi - lo) / 2u;
      if (b->free_spans[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }

   joins_prev = lo > 0
      && b->free_spans[lo - 1].offset + b->free_spans[lo - 1].size == offset;
   joins_next = lo < b->free_count && offset + size == b->free_spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->free_spans[lo - 1].size += size + b->free_spans[lo].size;
      size = b->free_spans[lo - 1].size;
      gs_vk_block_remove_span(b, lo);
   }
   else if (joins_prev)
   {
      b->free_spans[lo - 1].size += size;
      size = b->free_spans[lo - 1].size;
   }
   else if (joins_next)
   {
      b->free_spans[lo].offset = offset;
      b->free_spans[lo].size  += size;
      size = b->free_spans[lo].size;
   }
   else if (!gs_vk_block_insert_span(b, lo, offset, size))
      return;

   if (size > b->max_free)
      b->max_free = size;
}

/* A flush has to name a range that starts and ends on
 * nonCoherentAtomSize, so it is widened to one. Widening is safe: the
 * block around it is ours either way. */
static void gs_vk_heap_range(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc,
      VkMappedMemoryRange *range)
{
   const VkDeviceSize atom  = heap->atom_size;
   const VkDeviceSize begin = alloc->offset & ~(atom - 1);
   VkDeviceSize end         = gs_vk_align_up(alloc->offset + alloc->size, atom);
   const VkDeviceSize block = heap->blocks[alloc->block].size;

   if (end > block)
      end = block;

   memset(range, 0, sizeof(*range));
   range->sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
   range->memory = alloc->memory;
   range->offset = begin;
   range->size   = end - begin;
}

void gs_vk_heap_flush(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   VkMappedMemoryRange range;

   if (!alloc->mapped || !heap->fns.flush_ranges)
      return;
   if (heap->props.memoryTypes[alloc->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return;

   gs_vk_heap_range(heap, alloc, &range);
   heap->fns.flush_ranges(heap->device, 1, &range);
}

void gs_vk_heap_invalidate(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc)
{
   VkMappedMemoryRange range;

   if (!alloc->mapped || !heap->fns.invalidate_ranges)
      return;
   if (heap->props.memoryTypes[alloc->type].propertyFlags
         & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return;

   gs_vk_heap_range(heap, alloc, &range);
   heap->fns.invalidate_ranges(heap->device, 1, &range);
}
