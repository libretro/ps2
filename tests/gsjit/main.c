/* The software renderer's generated-code cache, on the real
 * GSFunctionMap.h and GSCodeReserve:
 *   - functions are placed only where the reserve has room; once it is
 *     full a request gets NULL, never room past its end;
 *   - a function longer than the first room it is given is generated
 *     again in more and comes back whole, and one longer than the most
 *     any function is given gets NULL;
 *   - after a reset a function asked for again is generated again: it is
 *     not the old address, which the next function now overwrites.
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void                 gsjit_assign(unsigned char* base, size_t size);
const unsigned char* gsjit_get(unsigned long long key);
void                 gsjit_reset(void);
void                 gsjit_release(void);

#define RESERVE (256u * 1024u)

static unsigned char* s_base;
static int s_failures;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("  FAIL: %s\n", what);
		s_failures++;
	}
}

static unsigned long long key(unsigned id, unsigned bytes)
{
	return ((unsigned long long)bytes << 32) | id;
}

/* The function for k is where it says and holds all its bytes. */
static int whole(const unsigned char* p, unsigned long long k)
{
	const size_t bytes = (size_t)(k >> 32);
	size_t i;
	if (!p || p < s_base || p + bytes > s_base + RESERVE)
		return 0;
	for (i = 0; i < bytes; i++)
		if (p[i] != (unsigned char)(k & 0xff))
			return 0;
	return 1;
}

int main(void)
{
	unsigned id, made = 0;
	const unsigned char *p, *first, *again;
	int past_end = 0, broken = 0, null_seen = 0;

	/* Room past the reserve, so a placement there is seen, not a crash. */
	s_base = (unsigned char*)calloc(1, RESERVE + 4 * 65536u);
	if (!s_base)
		return 2;
	gsjit_assign(s_base, RESERVE);

	/* Filling the reserve. */
	for (id = 1; id < 200; id++)
	{
		const unsigned long long k = key(id, 4000);
		p = gsjit_get(k);
		if (!p)
		{
			null_seen = 1;
			continue;
		}
		if (null_seen || p + 4000 > s_base + RESERVE)
			past_end = 1;
		else if (!whole(p, k))
			broken = 1;
		else
			made++;
	}
	check(null_seen, "a full reserve still hands out room");
	check(!past_end, "a function was placed past the end of the reserve");
	check(!broken, "a function in the reserve did not hold its bytes");
	check(made >= RESERVE / 4000 - 3, "the reserve ran out early");

	/* Long functions. */
	gsjit_reset();
	p = gsjit_get(key(201, 20000));
	check(whole(p, key(201, 20000)), "a function longer than its first room came back cut short");
	check(gsjit_get(key(202, 100000)) == NULL, "a function longer than any room was handed out");

	/* After a reset. */
	gsjit_reset();
	first = gsjit_get(key(203, 1000));
	gsjit_reset();
	again = gsjit_get(key(203, 1000));
	p = gsjit_get(key(204, 1000));
	(void)first;
	check(whole(again, key(203, 1000)) && whole(p, key(204, 1000)),
		"after a reset a function asked for again shares its code with another");

	gsjit_release();
	free(s_base);
	printf(s_failures ? "gs jit cache: FAILED (%d)\n" : "gs jit cache: ok, %u functions filled the reserve\n",
		s_failures ? (unsigned)s_failures : made);
	return s_failures != 0;
}
