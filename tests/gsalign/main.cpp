/* What the crash was: a CLUT inside a pooled object, compared with an
 * aligned SSE load. */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <emmintrin.h>
#include "GSObjectPool.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* GSVector4i::compare64 in miniature: cast and index, which is an
 * aligned load. */
static bool compare64(const void* a, const void* b, size_t size)
{
	const __m128i* x = (const __m128i*)a;
	const __m128i* y = (const __m128i*)b;
	size >>= 4;
	for (size_t i = 0; i < size; i++)
	{
		__m128i v = _mm_cmpeq_epi32(_mm_load_si128(x + i), _mm_load_si128(y + i));
		if (_mm_movemask_epi8(v) != 0xffff)
			return false;
	}
	return true;
}

struct Palette
{
	uint32_t m_alive;
	uint32_t m_refs;
	alignas(GS_OBJECT_POOL_ALIGN) uint32_t m_clut[256];
	uint16_t m_pal;
};

int main()
{
	gs_object_pool_t pool;
	int i;
	memset(&pool, 0, sizeof(pool));

	CHECK(alignof(Palette) >= 16, "the object is aligned enough for an SSE load");
	CHECK((offsetof(Palette, m_clut) % 16) == 0, "the CLUT is aligned inside it");

	gs_object_pool_init(&pool, sizeof(Palette), 64);
	for (i = 0; i < 64; i++)
	{
		Palette* p = (Palette*)gs_object_pool_take(&pool, sizeof(Palette));
		CHECK(p != NULL, "slot");
		CHECK(((uintptr_t)p % alignof(Palette)) == 0, "slot meets the object's alignment");
		CHECK(((uintptr_t)p->m_clut % 16) == 0, "so the CLUT is 16-byte aligned");
		memset(p->m_clut, i, sizeof(p->m_clut));
		/* The comparison that faulted. */
		CHECK(compare64(p->m_clut, p->m_clut, 1024), "compare a CLUT against itself");
	}
	gs_object_pool_free(&pool);
	printf(fails ? "align: FAILED (%d)\n" : "align: ok\n", fails);
	return fails != 0;
}
