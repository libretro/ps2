/*
 * Vulkan device memory, allocated in blocks at startup and suballocated
 * from there. In C.
 *
 * What it replaces is the Vulkan Memory Allocator, which is a good
 * library and the wrong shape for this. VMA decides when to ask the
 * driver for a VkDeviceMemory and how big, and those decisions are what
 * put this renderer on the floor: a dedicated allocation per render
 * target until the device ran out of allocation handles, blocks grown
 * on demand in the middle of a frame, and memory freed and re-taken
 * several times a second because the code above it asked for that.
 *
 * Here the rule is in one place and it is simple. Blocks are taken from
 * the driver, are large, and are never given back while the heap lives.
 * An allocation is an offset inside a block, found by walking that
 * block's free list; freeing puts the span back and merges it with its
 * neighbours. A host-visible block is mapped once when it is taken and
 * stays mapped, so nothing maps or unmaps per use either.
 *
 * Reserve at startup with gs_vk_heap_reserve and the frames that follow
 * ask the driver for nothing at all.
 *
 * Not thread safe; the GS is one thread.
 */

#ifndef GS_VULKAN_HEAP_H
#define GS_VULKAN_HEAP_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The entry points the heap uses, passed in rather than linked against,
 * because the loader that has them is the caller's. */
typedef struct gs_vk_heap_fns
{
   PFN_vkAllocateMemory              allocate_memory;
   PFN_vkFreeMemory                  free_memory;
   PFN_vkMapMemory                   map_memory;
   PFN_vkUnmapMemory                 unmap_memory;
   PFN_vkFlushMappedMemoryRanges     flush_ranges;
   PFN_vkInvalidateMappedMemoryRanges invalidate_ranges;
} gs_vk_heap_fns_t;

/* Where something ended up. offset is inside memory, not inside the
 * heap; mapped already has offset added, or is NULL for device-local
 * memory. */
typedef struct gs_vk_alloc
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
   unsigned       type;
} gs_vk_alloc_t;

/* A span of free memory inside a block. */
typedef struct gs_vk_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} gs_vk_span_t;

typedef struct gs_vk_block
{
   VkDeviceMemory memory;
   void          *mapped;      /* whole block, or NULL       */
   VkDeviceSize   size;
   VkDeviceSize   used;
   gs_vk_span_t  *free_spans;
   unsigned       free_count;
   unsigned       free_capacity;
   unsigned       type;
} gs_vk_block_t;

#define GS_VK_HEAP_MAX_BLOCKS 64

typedef struct gs_vk_heap
{
   VkDevice                         device;
   gs_vk_heap_fns_t                 fns;
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize                     block_size;
   VkDeviceSize                     atom_size;      /* nonCoherentAtomSize */
   gs_vk_block_t                    blocks[GS_VK_HEAP_MAX_BLOCKS];
   unsigned                         block_count;
   VkDeviceSize                     bytes_reserved; /* asked of the driver */
   VkDeviceSize                     bytes_used;     /* handed out          */
} gs_vk_heap_t;

/* block_size is what one VkDeviceMemory is; anything larger than it gets
 * a block of its own. Returns 0 on failure. */
int  gs_vk_heap_init(gs_vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props,
      const gs_vk_heap_fns_t *fns, VkDeviceSize block_size,
      VkDeviceSize non_coherent_atom_size);

void gs_vk_heap_shutdown(gs_vk_heap_t *heap);

/* Takes blocks from the driver now, so that later allocations of this
 * kind are offsets. type_bits and flags are as a VkMemoryRequirements
 * would give. Returns the number of blocks taken. */
unsigned gs_vk_heap_reserve(gs_vk_heap_t *heap, uint32_t type_bits,
      VkMemoryPropertyFlags flags, unsigned blocks);

/* Finds room for something with these requirements. required is what the
 * memory must be; preferred is tried first and dropped if no type has
 * it. Returns 0 if there is no room and no block could be taken. */
int  gs_vk_heap_alloc(gs_vk_heap_t *heap, const VkMemoryRequirements *req,
      VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
      gs_vk_alloc_t *out);

void gs_vk_heap_free(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);

/* For host-visible memory that is not coherent. Both round to
 * nonCoherentAtomSize themselves. */
void gs_vk_heap_flush(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);
void gs_vk_heap_invalidate(gs_vk_heap_t *heap, const gs_vk_alloc_t *alloc);

#ifdef __cplusplus
}
#endif

#endif
