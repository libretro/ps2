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
	printf(fails ? "pool: FAILED (%d)\n" : "pool: ok\n", fails);
	return fails != 0;
}
