/* The float-to-integer conversions GSdx makes on game-controlled values.
 *
 * Converting a float to an integer type is undefined for a NaN and for
 * anything outside the destination's range, and x86 and aarch64 produce
 * different answers rather than trapping, so the same draw takes a
 * different path depending on the host. The helpers in common/MathUtils.h
 * are what GSdx converts through instead; this checks that they are
 * defined everywhere, that they agree across hosts, and -- the property
 * that makes them safe to drop in -- that they are bit-identical to the
 * plain cast for every input the plain cast handles.
 *
 * Run it on both architectures. tests/gs/build.sh does that under qemu
 * when the cross tools are installed; a pass on one host proves nothing
 * on its own, since the point is that the two agree.
 *
 * Inputs go through a volatile. gcc folds these conversions at compile
 * time using its own saturating semantics, which are not the ISA's, and a
 * probe that lets it do so reports the opposite of what the CPU does.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "common/MathUtils.h"

static volatile float g_opaque;

static float opaque(float v)
{
	g_opaque = v;
	return g_opaque;
}

static int failures = 0;

static void check(const char *what, bool ok, const char *detail)
{
	if (!ok)
	{
		std::printf("  FAIL %-34s %s\n", what, detail);
		failures++;
	}
}

/* Every input worth naming: the boundaries of both destinations, the
 * values a degenerate Q produces, and the ones GSdx actually reaches --
 * a normalised depth scaled by 2^32, and the full unsigned 32-bit Z the
 * vertex trace holds as a float. */
static const struct
{
	const char *name;
	float v;
} INPUTS[] = {
	{ "0",                     0.0f },
	{ "-0",                    -0.0f },
	{ "1",                     1.0f },
	{ "-1",                    -1.0f },
	{ "255",                   255.0f },
	{ "65535",                 65535.0f },
	{ "2^23",                  8388608.0f },
	{ "2^31 - 128",            2147483520.0f },
	{ "2^31",                  2147483648.0f },
	{ "2^31 + 2^23",           2155872256.0f },
	{ "3e9",                   3000000000.0f },
	{ "2^32 - 256",            4294967040.0f },
	{ "2^32 (depth 1.0)",      4294967296.0f },
	{ "2^33",                  8589934592.0f },
	{ "-2^31",                 -2147483648.0f },
	{ "-2^31 - 256",           -2147483904.0f },
	{ "-2^32",                 -4294967296.0f },
	{ "1e30",                  1e30f },
	{ "-1e30",                 -1e30f },
};

int main(void)
{
	const float inf = opaque(1e30f) * 1e30f;
	const float nan = opaque(0.0f) / opaque(0.0f);
	unsigned i;

	std::printf("gs float->int conversions, %s\n",
#if defined(__x86_64__) || defined(__i386__)
		"x86"
#elif defined(__aarch64__)
		"aarch64"
#else
		"unknown arch"
#endif
	);

	/* 1. Defined and in range for every input, including the ones a
	 *    plain cast has no answer for. */
	for (i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++)
	{
		const float v = opaque(INPUTS[i].v);
		const s32 s = f32_to_s32_sat(v);
		const u32 u = f32_to_u32_sat(v);
		char d[128];

		/* A negative input must not come back as a huge unsigned; that
		 * is the -1.0 case, where the hosts disagree today. */
		std::snprintf(d, sizeof(d), "%s -> s32 %d, u32 %u", INPUTS[i].name, s, u);
		check("u32 of a negative is 0", !(v < 0.0f) || u == 0u, d);
		check("u32 of a large positive saturates",
		      !(v >= 4294967296.0f) || u == 4294967295u, d);
		check("s32 of a large positive saturates",
		      !(v >= 2147483648.0f) || s == 2147483647, d);
		check("s32 of a large negative saturates",
		      !(v < -2147483648.0f) || s == (-2147483647 - 1), d);
	}

	/* 2. Identical to the plain cast wherever the plain cast is defined.
	 *    Without this the helpers would be a behaviour change rather
	 *    than a repair, and every well-formed draw would shift. */
	for (i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++)
	{
		const float v = opaque(INPUTS[i].v);
		char d[128];

		if (v >= -2147483648.0f && v < 2147483648.0f)
		{
			const s32 want = (s32)v;
			std::snprintf(d, sizeof(d), "%s: sat %d, cast %d",
			              INPUTS[i].name, f32_to_s32_sat(v), want);
			check("s32 matches the plain cast in range",
			      f32_to_s32_sat(v) == want, d);
		}
		if (v >= 0.0f && v < 4294967296.0f)
		{
			const u32 want = (u32)v;
			std::snprintf(d, sizeof(d), "%s: sat %u, cast %u",
			              INPUTS[i].name, f32_to_u32_sat(v), want);
			check("u32 matches the plain cast in range",
			      f32_to_u32_sat(v) == want, d);
		}
	}

	/* 3. The three values a degenerate Q produces. NaN goes to zero in
	 *    both destinations; the infinities go to the ends. */
	check("s32 NaN is 0",      f32_to_s32_sat(nan) == 0, "NaN");
	check("u32 NaN is 0",      f32_to_u32_sat(nan) == 0u, "NaN");
	check("s32 +inf is max",   f32_to_s32_sat(inf) == 2147483647, "+inf");
	check("u32 +inf is max",   f32_to_u32_sat(inf) == 4294967295u, "+inf");
	check("s32 -inf is min",   f32_to_s32_sat(-inf) == (-2147483647 - 1), "-inf");
	check("u32 -inf is 0",     f32_to_u32_sat(-inf) == 0u, "-inf");

	/* 4. q_is_usable rejects exactly the Q values that cannot produce a
	 *    coordinate, and nothing else. */
	check("Q 0 unusable",       !q_is_usable(opaque(0.0f)),   "0");
	check("Q -0 unusable",      !q_is_usable(opaque(-0.0f)),  "-0");
	check("Q +inf unusable",    !q_is_usable(inf),            "+inf");
	check("Q -inf unusable",    !q_is_usable(-inf),           "-inf");
	check("Q NaN unusable",     !q_is_usable(nan),            "NaN");
	check("Q 1 usable",         q_is_usable(opaque(1.0f)),    "1");
	check("Q -1 usable",        q_is_usable(opaque(-1.0f)),   "-1");
	check("Q denormal usable",  q_is_usable(opaque(1e-40f)),  "1e-40");
	check("Q FLT_MIN usable",   q_is_usable(opaque(1.17549435e-38f)), "FLT_MIN");

	/* 5. The two sites this was written for, spelled out, so a
	 *    regression names itself rather than showing up as a colour. */
	{
		/* GSTextureCache::ConvertDepthToColor -- a normalised depth of
		 * exactly 1.0 is the usual far-plane clear. */
		const u32 cc = f32_to_u32_sat(opaque(1.0f) * 4294967296.0f);
		char d[64];
		std::snprintf(d, sizeof(d), "got 0x%08x", cc);
		check("depth 1.0 converts to white", cc == 0xffffffffu, d);
	}
	{
		/* GSRendererHW -- Z as (float)(u32), top byte wanted as alpha. */
		static const struct { u32 z; u32 alpha; } zs[] = {
			{ 0x00000000u, 0x00u }, { 0x7f000000u, 0x7fu },
			{ 0x80000000u, 0x80u }, { 0xff000000u, 0xffu },
		};
		unsigned k;
		for (k = 0; k < sizeof(zs) / sizeof(zs[0]); k++)
		{
			const u32 got = f32_to_u32_sat(opaque((float)zs[k].z)) >> 24;
			char d[64];
			std::snprintf(d, sizeof(d), "Z 0x%08x -> %u, want %u",
			              zs[k].z, got, zs[k].alpha);
			check("Z top byte as alpha", got == zs[k].alpha, d);
		}
	}

	if (failures == 0)
		std::printf("PASS: conversions defined, host-independent, and "
		            "unchanged where the plain cast was defined\n");
	else
		std::printf("FAIL: %d\n", failures);
	return failures != 0;
}
