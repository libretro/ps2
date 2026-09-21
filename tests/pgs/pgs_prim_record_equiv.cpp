/* Primitive-record invalidation lane.
 *
 * The per-primitive record is now cached and rebuilt only when
 * STATE_DIRTY_PRIM_TEMPLATE_BIT fires. That is only correct if every register
 * the record reads dirties that bit: FRAME, ALPHA, TEST and FOGCOL in both
 * contexts, and PRIM when it switches context.
 *
 * This drives a randomised stream of register writes and primitive kicks
 * through two models -- one rebuilding the record from the registers every
 * primitive, one using the cache -- and requires identical records. A writer
 * that forgets to raise the bit shows up as a divergence.
 */
#include "common.h"
#include <cstdio>
#include <cstring>

/* the dirty bit under test */
enum { DIRTY_PRIM_TEMPLATE = 1u << 0, DIRTY_OTHER = 1u << 1 };

/* the register state the record is built from */
struct Ctx { uint32_t fbmsk, alpha_fix, aref; };
struct Regs2 {
	Ctx ctx[2];
	uint32_t fogcol;
	uint32_t ctxt;       /* PRIM.CTXT */
	uint32_t tex, tex2, state;   /* stand-ins for prim_template's fields */
};
struct Record { uint32_t tex, tex2, state, alpha, fbmsk, fogcol; };

static Regs2 R2;
static uint32_t DIRTY;

/* Build the record from the registers -- the reference. */
static void build(Record &r)
{
	const Ctx &c = R2.ctx[R2.ctxt];
	r.tex = R2.tex; r.tex2 = R2.tex2; r.state = R2.state;
	r.fbmsk = c.fbmsk;
	r.fogcol = R2.fogcol;
	r.alpha = (c.alpha_fix << 0) | (c.aref << 8);
}

/* The cache, refreshed only under the dirty bit. */
static Record CACHE;
static void refresh_if_dirty(void)
{
	if (!(DIRTY & DIRTY_PRIM_TEMPLATE))
		return;
	DIRTY &= ~DIRTY_PRIM_TEMPLATE;
	build(CACHE);
}

/* The register writers, each raising what the real handler raises.
 * FOGCOL_RAISES is the thing under test. */
#ifndef FOGCOL_RAISES
#define FOGCOL_RAISES 1
#endif

static void w_frame(uint32_t c, uint32_t v){ R2.ctx[c].fbmsk = v; DIRTY |= DIRTY_PRIM_TEMPLATE; }
static void w_alpha(uint32_t c, uint32_t v){ R2.ctx[c].alpha_fix = v & 0xff; DIRTY |= DIRTY_PRIM_TEMPLATE; }
static void w_test (uint32_t c, uint32_t v){ R2.ctx[c].aref = v & 0xff; DIRTY |= DIRTY_PRIM_TEMPLATE; }
static void w_fogcol(uint32_t v)
{
	R2.fogcol = v;
#if FOGCOL_RAISES
	DIRTY |= DIRTY_PRIM_TEMPLATE;
#endif
}
static void w_prim(uint32_t new_ctxt)
{
	if (new_ctxt != R2.ctxt) DIRTY |= DIRTY_PRIM_TEMPLATE;
	R2.ctxt = new_ctxt;
}
/* a write that touches nothing the record reads */
static void w_unrelated(void){ DIRTY |= DIRTY_OTHER; }

/* prim_template itself is rebuilt under the same bit */
static void w_template(uint32_t t, uint32_t t2, uint32_t st)
{
	R2.tex = t; R2.tex2 = t2; R2.state = st;
	DIRTY |= DIRTY_PRIM_TEMPLATE;
}

int main(void)
{
	uint64_t x = 0x243F6A8885A308D3ull;
	long prims = 0, fails = 0;
	int i;

	memset(&R2, 0, sizeof R2);
	DIRTY = DIRTY_PRIM_TEMPLATE;
	build(CACHE);

	for (i = 0; i < 40000000; i++) {
		uint32_t op, v, c;
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		op = (uint32_t)(x % 9);
		v = (uint32_t)(x >> 8);
		c = (uint32_t)((x >> 4) & 1);

		switch (op) {
		case 0: w_frame(c, v); break;
		case 1: w_alpha(c, v); break;
		case 2: w_test(c, v); break;
		case 3: w_fogcol(v); break;
		case 4: w_prim(c); break;
		case 5: w_template(v, v ^ 0x55, v >> 3); break;
		case 6: w_unrelated(); break;
		default: {
			/* a primitive kick */
			Record ref, got;
			refresh_if_dirty();
			got = CACHE;
			build(ref);
			prims++;
			if (memcmp(&ref, &got, sizeof ref) != 0) {
				if (fails < 3)
					printf("MISMATCH at step %d: fogcol ref %08x got %08x, fbmsk %08x/%08x, alpha %08x/%08x\n",
					       i, ref.fogcol, got.fogcol, ref.fbmsk, got.fbmsk, ref.alpha, got.alpha);
				fails++;
			}
			break;
		}
		}
	}
	printf("%s: primitive-record invalidation, %ld records compared, %ld failures\n",
	       fails ? "FAIL" : "PASS", prims, fails);
	return fails != 0;
}
