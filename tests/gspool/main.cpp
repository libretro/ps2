/* The texture cache's object pool (GS/Renderers/HW/GSObjectPool.c), the
 * real one: slots handed out once each, aligned, not overlapping,
 * returned and reused, and NULL past the pool's size so the caller can
 * fall back. */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include "GS/Renderers/HW/GSObjectPool.h"

#define POOL 384
static gs_object_pool_t pool_obj;
static void* take(size_t size)
{
	void* p;
	if (!pool_obj.slots)
		gs_object_pool_init(&pool_obj, size, POOL);
	p = gs_object_pool_take(&pool_obj, size);
	return p ? p : memalign_alloc(32, size);
}
static void give(void* p)
{
	if (p && !gs_object_pool_give(&pool_obj, p))
		memalign_free(p);
}

static int fails;
#define CHECK(c,m) do{ if(!(c)){ printf("  FAIL: %s\n", m); fails++; } }while(0)
int main()
{
	const size_t sz = 3472;
	std::set<void*> live;
	void* p[POOL+16];
	int i, pass;
	for (pass = 0; pass < 3; pass++)
	{
		live.clear();
		for (i = 0; i < POOL; i++)
		{
			p[i] = take(sz);
			CHECK(p[i] != NULL, "pool slot handed out");
			CHECK(((uintptr_t)p[i] & 31) == 0, "slot is 32-byte aligned");
			CHECK(live.insert(p[i]).second, "slot not handed out twice");
			memset(p[i], 0xA5, sz);           /* the whole slot is ours */
		}
		/* Past the pool: still works, from the allocator. */
		for (i = 0; i < 16; i++)
		{
			p[POOL + i] = take(sz);
			CHECK(p[POOL + i] != NULL, "overflow allocation");
			CHECK(live.insert(p[POOL + i]).second, "overflow is distinct");
			memset(p[POOL + i], 0x5A, sz);
		}
		for (i = POOL + 15; i >= 0; i--)
			give(p[i]);
	}
	/* Slots do not overlap: write a marker in each, then check them all. */
	for (i = 0; i < POOL; i++) { p[i] = take(sz); memset(p[i], i & 0xff, sz); }
	for (i = 0; i < POOL; i++)
	{
		unsigned char* q = (unsigned char*)p[i];
		CHECK(q[0] == (i & 0xff) && q[sz - 1] == (i & 0xff), "slot contents intact");
	}
	for (i = 0; i < POOL; i++) give(p[i]);
	/* A second free of the same object must not put the slot on the free
	 * list twice: that hands one slot to two callers, and a live object
	 * gets overwritten by an unrelated one - which shows up as the same
	 * address being destroyed over and over, and then a lockup.
	 *
	 * paraLLEl-GS cannot have this bug, because it owns images through a
	 * refcounted handle and a second free is not something a call site
	 * can express. Sources and targets here are raw pointers. */
	{
		gs_object_pool_t dbl;
		void* x;
		void* y;
		void* z;
		memset(&dbl, 0, sizeof(dbl));
		gs_object_pool_init(&dbl, 64, 4);
		x = gs_object_pool_take(&dbl, 64);
		CHECK(x != NULL, "took one");
		CHECK(gs_object_pool_give(&dbl, x) == 1, "gave it back");
		CHECK(gs_object_pool_give(&dbl, x) == 1, "gave it back twice, which is a caller bug");
		CHECK(dbl.double_frees == 1, "and the pool counted it rather than corrupting itself");
		y = gs_object_pool_take(&dbl, 64);
		z = gs_object_pool_take(&dbl, 64);
		CHECK(y != z, "two takes never hand out one slot");
		gs_object_pool_free(&dbl);
	}

	printf(fails ? "pool: FAILED (%d)\n" : "pool: ok\n", fails);
	return fails != 0;
}
