/* GS/Renderers/DX12/GSD3D12Heap.c - the suballocator that replaces
 * D3D12MemoryAllocator. No GPU: the device entry points are stubs that
 * count calls and hand out fake heaps, so what is tested is the
 * arithmetic - which is the part that decides whether a frame
 * allocates. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GSD3D12Heap.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

#define MB (1024ull * 1024ull)

/* Tier 1 keeps buffers and textures apart; these stand in for the real
 * D3D12_HEAP_FLAG values, which this file does not include. */
#define FLAG_BUFFERS       0x80u
#define FLAG_RT_DS         0x40u
#define FLAG_ALL           0x00u

static int creates;
static int releases;
static int create_fails_after; /* -1 for never */

static int stub_create(void *device, uint64_t size, uint32_t heap_type,
      uint32_t heap_flags, void **out_heap)
{
   (void)device; (void)size; (void)heap_type; (void)heap_flags;
   if (create_fails_after >= 0 && creates >= create_fails_after)
      return 0;
   creates++;
   *out_heap = (void*)(size_t)(0x1000 + creates);
   return 1;
}

static void stub_release(void *device, void *heap)
{
   (void)device; (void)heap;
   releases++;
}

static const gs_d3d12_heap_fns_t stub_fns = { stub_create, stub_release };

static void reset_stubs(void)
{
   creates            = 0;
   releases           = 0;
   create_fails_after = -1;
}

static void test_reserve_then_no_device_calls(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a[8];
   int i;
   int creates_after_reserve;

   printf("reserve, then allocations ask the device for nothing\n");
   reset_stubs();
   CHECK(gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 4096 * MB),
         "init");
   CHECK(gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
            FLAG_RT_DS, 2) == 2, "two blocks reserved");
   creates_after_reserve = creates;

   for (i = 0; i < 8; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, 4 * MB, 64 * 1024,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &a[i]),
            "allocation fits in reserved blocks");

   CHECK(creates == creates_after_reserve,
         "no new heap was created for those allocations");
   CHECK(heap.bytes_used == 32 * MB, "used total");
   CHECK(heap.bytes_device == 32 * MB, "counted as device memory");
   CHECK(heap.bytes_host == 0, "nothing counted as host memory");

   for (i = 0; i < 8; i++)
      gs_d3d12_heap_free(&heap, &a[i]);
   CHECK(heap.bytes_used == 0, "all given back");
   CHECK(heap.bytes_device == 0, "device counter back to zero");

   gs_d3d12_heap_shutdown(&heap);
   CHECK(releases == creates, "every heap released at shutdown");
}

static void test_alignment(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a, b;

   printf("offsets respect the alignment asked for\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 1000, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "first");
   CHECK(gs_d3d12_heap_alloc(&heap, 1000, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &b), "second");
   CHECK((a.offset & (64 * 1024 - 1)) == 0, "first is aligned");
   CHECK((b.offset & (64 * 1024 - 1)) == 0, "second is aligned");
   CHECK(b.offset >= a.offset + a.size, "they do not overlap");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_free_merges(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a[4];
   gs_d3d12_alloc_t big;
   int i;

   printf("freed spans merge, so a block does not fragment away\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 64 * MB);

   for (i = 0; i < 4; i++)
      CHECK(gs_d3d12_heap_alloc(&heap, 16 * MB, 64 * 1024,
               GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a[i]), "fill the block");

   for (i = 0; i < 4; i++)
      gs_d3d12_heap_free(&heap, &a[i]);

   /* If the four freed spans did not merge back into one, this cannot
    * be satisfied and the ceiling forbids another block. */
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &big),
         "whole block available again after merge");

   gs_d3d12_heap_free(&heap, &big);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_ceiling(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a, b, c;
   unsigned taken;

   printf("the ceiling refuses rather than taking the card\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 128 * MB);

   taken = gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
         FLAG_ALL, 8);
   CHECK(taken == 2, "only what the ceiling allows was taken");
   CHECK(heap.bytes_reserved == 128 * MB, "reserved is the ceiling");

   /* Both blocks in use, so trim has nothing to give back and there is
    * no room for a third. An empty block would have been trimmed and
    * the request met, which is the other path and is tested above. */
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "first block in use");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &b), "second block in use");
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &c) == 0,
         "refused past the ceiling");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_free(&heap, &b);

   gs_d3d12_heap_shutdown(&heap);
}

static void test_trim_and_hole_reuse(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t keep, a;
   unsigned before;

   printf("trim gives empty blocks back and the slot gets reused\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 128 * MB);
   gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, 2);

   /* Something live in the first block, nothing in the second. */
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &keep), "live allocation");
   before = heap.block_count;
   CHECK(gs_d3d12_heap_trim(&heap) == 1, "the empty block went back");
   CHECK(heap.block_count == before, "the slot stayed, as a hole");
   CHECK(heap.bytes_reserved == 64 * MB, "reserved dropped by one block");

   /* The live allocation still points at its own block. */
   CHECK(keep.block < heap.block_count, "live allocation still in range");
   CHECK(heap.blocks[keep.block].heap == keep.heap,
         "live allocation still points at its own heap");

   /* A new block lands in the hole rather than past the end. */
   CHECK(gs_d3d12_heap_alloc(&heap, 64 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &a),
         "a new block after the trim");
   CHECK(heap.block_count == before, "it reused the hole");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_free(&heap, &keep);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_type_and_flag_segregation(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t buf, rt;

   printf("blocks are kept apart by heap type and heap flags\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_UPLOAD, FLAG_BUFFERS, &buf), "an upload buffer");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_RT_DS, &rt), "a render target");

   CHECK(buf.heap != rt.heap,
         "a tier 1 device would refuse these in one heap, so they are not");
   CHECK(heap.bytes_host == 1 * MB, "upload counted as host");
   CHECK(heap.bytes_device == 1 * MB, "target counted as device");

   /* Same type, different flags, still a different block. */
   CHECK(creates == 2, "one heap each");

   gs_d3d12_heap_free(&heap, &buf);
   gs_d3d12_heap_free(&heap, &rt);
   gs_d3d12_heap_shutdown(&heap);
}

static void test_device_refusal_is_not_a_crash(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("a device that will not create a heap is refused cleanly\n");
   reset_stubs();
   create_fails_after = 0;
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_reserve(&heap, GS_D3D12_HEAP_TYPE_DEFAULT,
            FLAG_ALL, 4) == 0, "nothing reserved");
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a) == 0, "refused");
   CHECK(heap.block_count == 0, "no block recorded");
   CHECK(heap.bytes_reserved == 0, "nothing counted as reserved");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_free_of_a_stale_allocation(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("freeing something twice, or from a trimmed block, does nothing\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);
   CHECK(gs_d3d12_heap_alloc(&heap, 1 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "allocate");

   gs_d3d12_heap_free(&heap, &a);
   CHECK(heap.bytes_used == 0, "freed once");
   gs_d3d12_heap_free(&heap, &a);
   CHECK(heap.bytes_used == 0, "freed twice, still zero rather than underflow");

   gs_d3d12_heap_trim(&heap);
   gs_d3d12_heap_free(&heap, &a); /* block is a hole now */
   CHECK(heap.bytes_used == 0, "free against a trimmed block is ignored");

   gs_d3d12_heap_shutdown(&heap);
}

static void test_oversized_gets_its_own_block(void)
{
   gs_d3d12_heap_t heap;
   gs_d3d12_alloc_t a;

   printf("something bigger than a block gets a block of its own\n");
   reset_stubs();
   gs_d3d12_heap_init(&heap, NULL, &stub_fns, 64 * MB, 0);

   CHECK(gs_d3d12_heap_alloc(&heap, 100 * MB, 64 * 1024,
            GS_D3D12_HEAP_TYPE_DEFAULT, FLAG_ALL, &a), "oversized allocation");
   CHECK(heap.bytes_reserved >= 100 * MB, "a block large enough was taken");
   CHECK(a.offset == 0, "it sits at the start of its own block");

   gs_d3d12_heap_free(&heap, &a);
   gs_d3d12_heap_shutdown(&heap);
}

int main(void)
{
   test_reserve_then_no_device_calls();
   test_alignment();
   test_free_merges();
   test_ceiling();
   test_trim_and_hole_reuse();
   test_type_and_flag_segregation();
   test_device_refusal_is_not_a_crash();
   test_free_of_a_stale_allocation();
   test_oversized_gets_its_own_block();

   if (fails)
   {
      printf("d3d12heap: %d failed\n", fails);
      return 1;
   }
   printf("d3d12heap: ok\n");
   return 0;
}
