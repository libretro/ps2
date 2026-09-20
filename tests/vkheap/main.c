/* GS/Renderers/Vulkan/GSVulkanHeap.c - the suballocator that replaces
 * VMA. No GPU: the Vulkan entry points are stubs that count calls and
 * hand out fake handles, so what is tested is the arithmetic - which is
 * the part that decides whether a frame allocates. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GSVulkanHeap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int allocations;   /* how many times the driver was asked */
static int frees;
static int maps;

static VkResult stub_allocate(VkDevice d, const VkMemoryAllocateInfo *ai,
      const VkAllocationCallbacks *cb, VkDeviceMemory *out)
{
   (void)d; (void)cb;
   allocations++;
   *out = (VkDeviceMemory)(uintptr_t)(0x1000 + allocations);
   (void)ai;
   return VK_SUCCESS;
}

static void stub_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb)
{
   (void)d; (void)m; (void)cb;
   frees++;
}

/* What the stub handed out for each fake VkDeviceMemory, so unmap can
 * give it back - otherwise the test leaks and the leak is the test's. */
static VkDeviceMemory mapped_handles[GS_VK_HEAP_MAX_BLOCKS];
static void          *mapped_ptrs[GS_VK_HEAP_MAX_BLOCKS];
static unsigned       mapped_n;

static VkResult stub_map(VkDevice d, VkDeviceMemory m, VkDeviceSize off,
      VkDeviceSize size, VkMemoryMapFlags f, void **out)
{
   (void)d; (void)off; (void)f;
   maps++;
   *out = malloc(size == VK_WHOLE_SIZE ? (4u * 1024u * 1024u) : (size_t)size);
   if (!*out)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (mapped_n < GS_VK_HEAP_MAX_BLOCKS)
   {
      mapped_handles[mapped_n] = m;
      mapped_ptrs[mapped_n]    = *out;
      mapped_n++;
   }
   return VK_SUCCESS;
}

static void stub_unmap(VkDevice d, VkDeviceMemory m)
{
   unsigned i;
   (void)d;
   for (i = 0; i < mapped_n; i++)
   {
      if (mapped_handles[i] != m)
         continue;
      free(mapped_ptrs[i]);
      mapped_handles[i] = mapped_handles[mapped_n - 1];
      mapped_ptrs[i]    = mapped_ptrs[mapped_n - 1];
      mapped_n--;
      return;
   }
}

static VkResult stub_flush(VkDevice d, uint32_t n, const VkMappedMemoryRange *r)
{
   (void)d; (void)n; (void)r;
   return VK_SUCCESS;
}

static void fill_props(VkPhysicalDeviceMemoryProperties *p)
{
   memset(p, 0, sizeof(*p));
   p->memoryHeapCount = 1;
   p->memoryHeaps[0].size = 1024u * 1024u * 1024u;
   p->memoryTypeCount = 2;
   /* 0: device local. 1: host visible and coherent. */
   p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   p->memoryTypes[0].heapIndex = 0;
   p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   p->memoryTypes[1].heapIndex = 0;
}

static void req(VkMemoryRequirements *r, VkDeviceSize size, VkDeviceSize align, uint32_t bits)
{
   r->size = size;
   r->alignment = align;
   r->memoryTypeBits = bits;
}

int main(void)
{
   VkPhysicalDeviceMemoryProperties props;
   gs_vk_heap_fns_t fns;
   gs_vk_heap_t heap;
   VkMemoryRequirements r;
   gs_vk_alloc_t a[64];
   int i;

   fill_props(&props);
   memset(&fns, 0, sizeof(fns));
   fns.allocate_memory = stub_allocate;
   fns.free_memory     = stub_free;
   fns.map_memory      = stub_map;
   fns.unmap_memory    = stub_unmap;
   fns.flush_ranges    = stub_flush;

   CHECK(gs_vk_heap_init(&heap, (VkDevice)1, &props, &fns, 1024 * 1024, 256) != 0, "init");

   /* Reserving takes blocks now. */
   allocations = 0;
   CHECK(gs_vk_heap_reserve(&heap, 0x3u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2) == 2,
         "reserve takes the blocks it was asked for");
   CHECK(allocations == 2, "two driver allocations, no more");

   /* And then nothing does. This is the whole point. */
   allocations = 0;
   req(&r, 64 * 1024, 256, 0x1u);
   for (i = 0; i < 16; i++)
      CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a[i]) != 0,
            "allocation out of the reserved blocks");
   CHECK(allocations == 0, "sixteen allocations, nothing asked of the driver");

   /* Offsets do not overlap, and honour alignment. */
   for (i = 0; i < 16; i++)
   {
      int j;
      CHECK((a[i].offset % 256) == 0, "aligned");
      for (j = i + 1; j < 16; j++)
      {
         const int same = (a[i].memory == a[j].memory);
         const int apart = (a[i].offset + a[i].size <= a[j].offset)
            || (a[j].offset + a[j].size <= a[i].offset);
         CHECK(!same || apart, "two allocations never overlap");
      }
   }

   /* Freeing and re-taking the same size reuses the space rather than
    * growing: the churn case, which is what put the card on the floor. */
   for (i = 0; i < 16; i++)
      gs_vk_heap_free(&heap, &a[i]);
   CHECK(heap.bytes_used == 0, "everything given back");

   allocations = 0;
   for (i = 0; i < 16; i++)
      CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a[i]) != 0,
            "re-taken");
   CHECK(allocations == 0, "a free and re-take asks the driver for nothing");

   /* Fragmentation: free every other one, then ask for something that
    * needs two adjacent - it must come from the merged space, still
    * without the driver. */
   for (i = 0; i < 16; i += 2)
      gs_vk_heap_free(&heap, &a[i]);
   for (i = 1; i < 16; i += 2)
      gs_vk_heap_free(&heap, &a[i]);
   allocations = 0;
   req(&r, 512 * 1024, 256, 0x1u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a[0]) != 0,
         "a large allocation after freeing everything");
   CHECK(allocations == 0, "freed spans merged, so no new block");
   gs_vk_heap_free(&heap, &a[0]);

   /* Host visible memory is mapped once, per block, and an allocation's
    * pointer is inside that mapping. */
   maps = 0;
   req(&r, 4096, 64, 0x2u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, &a[0]) != 0,
         "host visible allocation");
   CHECK(a[0].mapped != NULL, "and it is mapped");
   CHECK(maps == 1, "the block was mapped once");
   maps = 0;
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, &a[1]) != 0,
         "a second one");
   CHECK(maps == 0, "no second mapping");
   CHECK(a[1].mapped != a[0].mapped, "different offsets, different pointers");

   /* Something bigger than a block gets its own. */
   allocations = 0;
   req(&r, 4u * 1024u * 1024u, 256, 0x1u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a[2]) != 0,
         "an allocation larger than the block size");
   CHECK(allocations == 1, "which takes exactly one block");

   /* A requirement no memory type satisfies fails rather than pretending. */
   req(&r, 1024, 256, 0x0u);
   CHECK(gs_vk_heap_alloc(&heap, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &a[3]) == 0,
         "no type, no allocation");

   frees = 0;
   gs_vk_heap_shutdown(&heap);
   CHECK(frees > 0, "shutdown gives the blocks back");
   CHECK(heap.block_count == 0, "and forgets them");
   CHECK(mapped_n == 0, "and unmapped every block it had mapped");

   printf(fails ? "vkheap: FAILED (%d)\n" : "vkheap: ok\n", fails);
   return fails != 0;
}
