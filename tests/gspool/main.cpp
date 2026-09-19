/* The source pool's slot arithmetic, on its own: take every slot, give
 * them back, take them again, and overflow past the pool. */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#define POOL 384
static unsigned char* pool; static size_t stride;
static unsigned short freelist[POOL]; static unsigned freecount;
static void* take(size_t size)
{
	if (!pool) { unsigned i; stride=(size+31)&~(size_t)31;
		pool=(unsigned char*)memalign_alloc(32, stride*POOL);
		for (i=0;i<POOL;i++) freelist[i]=(unsigned short)(POOL-1-i);
		freecount=POOL; }
	if (freecount && size<=stride) return pool + (size_t)freelist[--freecount]*stride;
	return memalign_alloc(32, size);
}
static void give(void* p)
{
	if (!p) return;
	if (pool && (unsigned char*)p>=pool && (unsigned char*)p<pool+stride*POOL)
	{ size_t off=(size_t)((unsigned char*)p-pool);
	  if (freecount<POOL) freelist[freecount++]=(unsigned short)(off/stride);
	  return; }
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
