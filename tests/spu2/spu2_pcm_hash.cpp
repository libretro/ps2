/* SPU2 output, hashed.
 *
 * The mixer is deterministic: the same register writes over the same sample
 * RAM produce the same PCM, sample for sample. So a change to it is either
 * bit-exact or it is not, and this says which.
 *
 * The SPU2 is driven here the way a game drives it -- voices pointed at
 * ADPCM blocks in sample RAM, pitch, ADSR envelopes, volumes and their
 * slides, the noise generator, reverb, and the dry/wet gates -- and every
 * stereo sample Mix() emits is folded into a hash. Run it before a change
 * and after; the numbers match or the change altered what a game hears.
 *
 * The scenarios are separate hashes rather than one, so a mismatch names
 * the area that moved instead of only saying something did.
 *
 * What it catches is measured, by injecting faults into Mixer.cpp and
 * checking the hashes move. Seven of eight do: both saturation bounds, the
 * core 0 and core 1 input volumes, the core 0 master volume, a dry gate
 * reading the wrong channel, the DC filter coefficient and the final output
 * clamp.
 *
 * The eighth does not, and the reason is worth stating so nobody assumes
 * otherwise. Changing the per-voice wet clamp leaves every hash alone, even
 * moved as far as -0x4000. The clamp is reached -- the accumulator runs to
 * around -227000, thousands of samples past the bound -- but what it feeds
 * is the reverb return, and that arrives at about a hundredth of the dry
 * path: RV peaks near 383 against a dry signal at full scale. The
 * difference does not survive into 16 bits. Closing it needs a scenario
 * where the wet path carries the signal on its own, which needs reverb
 * parameters that produce steady output rather than occasional bursts.
 * Until then a change to those two lines wants checking another way.
 *
 * Note also what Init leaves shut: DryGate.ExtL and ExtR are zero on both
 * cores, and core 0 reaches the output only as core 1's Ext input. A
 * scenario that does not open that gate tests one core while appearing to
 * test two, which is why open_ext_path() exists.
 */

#include "Global.h"
#include "spu2.h"
#include "Dma.h"
#include "R3000A.h"
#include "IopCounters.h"
#include "MemoryTypes.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

/* ------------------------------------------------------------------ */
/* What the SPU2 expects the rest of the emulator to provide. None of   */
/* it participates in mixing: the IOP side raises interrupts and moves  */
/* DMA, and the audio side takes finished samples away. Here the        */
/* samples are taken by the harness instead.                            */
/* ------------------------------------------------------------------ */

alignas(__pagealignsize) u8 iopHw[Ps2MemSize::IopHardware];
IopVM_MemoryAllocMess* iopMem;
alignas(16) psxRegisters psxRegs;
psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter;
u32 psxNextStartCounter;
u32 lClocks;

void psxDmaInterrupt(int) {}
void psxDmaInterrupt2(int) {}
void spu2Irq(void) {}

/* The sink. Mix() is called directly, so these are only reached if the
 * core tries to hand samples off on its own. */
s16 *retro_audio_reserve(int) { static s16 buf[4096]; return buf; }
void retro_audio_commit(int) {}

/* ------------------------------------------------------------------ */
/* A hash with no library behind it, so the number is reproducible
 * anywhere this builds. FNV-1a over the little-endian sample pairs. */
/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_s16(uint64_t *h, s16 v)
{
	uint16_t u = (uint16_t)v;
	*h = (*h ^ (uint64_t)(u & 0xff))      * 1099511628211ull;
	*h = (*h ^ (uint64_t)((u >> 8) & 0xff)) * 1099511628211ull;
}

/* ------------------------------------------------------------------ */
/* Deterministic setup.                                                 */
/* ------------------------------------------------------------------ */

static uint32_t rs = 0x12345678u;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* ADPCM blocks: 16 bytes each, a header byte pair then 14 of nibble
 * pairs. Filled with a mix of shapes so decode, loop and end flags are
 * all exercised rather than one flat tone. */
static void fill_sample_ram(void)
{
	u32 addr;
	rs = 0x12345678u;
	memset(_spu2mem, 0, sizeof(_spu2mem));

	for (addr = 0x1000; addr < 0x80000; addr += 8)
	{
		u8 *blk = (u8 *)(_spu2mem + addr);
		int i;
		/* shift 0..12 in the low nibble, filter 0..4 in the high */
		blk[0] = (u8)(((rnd() % 5) << 4) | (rnd() % 13));
		/* loop start on some blocks, loop-end + repeat on others, so a
		 * voice started anywhere finds a terminator eventually */
		blk[1] = (u8)((addr % 0x400) == 0 ? 0x06 : ((addr % 0x200) == 0 ? 0x01 : 0x00));
		for (i = 2; i < 16; i++)
			blk[i] = (u8)rnd();
	}
}

static void reset_core(void)
{
	memset(spu2regs, 0, sizeof(spu2regs));
	memset(&DCFilterIn, 0, sizeof(DCFilterIn));
	memset(&DCFilterOut, 0, sizeof(DCFilterOut));
	Cores[0].Init(0);
	Cores[1].Init(1);
	OutPos = 0;
	InputPos = 0;
	Cycles = 0;
	PlayMode = 0;
}

/* Core 0's output arrives at the final mix as core 1's Ext input, and Init
 * closes that gate on both cores. Left shut, nothing core 0 does -- its
 * voices, its input, its master volume -- reaches the output at all, and a
 * scenario silently tests one core instead of two. */
static void open_ext_path(void)
{
	Cores[1].DryGate.ExtL = -1;
	Cores[1].DryGate.ExtR = -1;
	Cores[1].WetGate.ExtL = -1;
	Cores[1].WetGate.ExtR = -1;
	Cores[1].ExtVol.Left  = 0x7fff;
	Cores[1].ExtVol.Right = 0x7fff;
}

/* Start a voice on a block of sample RAM with a given pitch and envelope. */
static void start_voice(int core, int v, u32 addr, u16 pitch,
                        u16 adsr1, u16 adsr2, s16 vl, s16 vr)
{
	V_Voice& vc = Cores[core].Voices[v];
	vc.StartA        = addr & ~7u;
	vc.NextA         = vc.StartA;
	vc.LoopStartA    = vc.StartA;
	vc.Pitch         = pitch;
	vc.ADSR.regADSR1 = adsr1;
	vc.ADSR.regADSR2 = adsr2;
	vc.Volume.Left.Value   = vl;
	vc.Volume.Right.Value  = vr;
	vc.Volume.Left.Enable  = 0;
	vc.Volume.Right.Enable = 0;

	/* What the KeyOn path in SPU2async does. Mix() does not run it, so
	 * the harness puts the voice in the state a key-on leaves it in. */
	vc.ADSR.Phase   = PHASE_ATTACK;
	vc.ADSR.Counter = 0;
	vc.ADSR.Value   = 0;
	ADSR_UpdateCache(vc.ADSR);
	vc.SCurrent  = 28;
	vc.LoopMode  = 0;
	vc.SP        = -1;
	vc.LoopFlags = 0;
	vc.NextA     = vc.StartA | 1;
	vc.Prev1 = vc.Prev2 = 0;
	vc.PV1 = vc.PV2 = vc.PV3 = vc.PV4 = 0;
	Cores[core].Regs.ENDX &= ~(1u << v);
}

/* ------------------------------------------------------------------ */
/* Scenarios.                                                           */
/* ------------------------------------------------------------------ */

/* Set by run(), so a scenario can be shown to be mixing audio rather than
 * producing a very reproducible silence. */
static long   g_nonzero;
static int    g_peak;
static double g_rms;
static long   g_hi_rail;   /* samples at +0x7fff */
static long   g_lo_rail;   /* samples at -0x8000 */

static uint64_t run(int samples)
{
	uint64_t h = hash_init();
	double acc = 0.0;
	int i;

	g_nonzero = 0;
	g_peak = 0;
	g_hi_rail = 0;
	g_lo_rail = 0;

	for (i = 0; i < samples; i++)
	{
		s16 l = 0, r = 0;
		int a;
		Mix(&l, &r);
		hash_s16(&h, l);
		hash_s16(&h, r);
		if (l || r) g_nonzero++;
		if (l ==  0x7fff || r ==  0x7fff) g_hi_rail++;
		if (l == -0x8000 || r == -0x8000) g_lo_rail++;
		a = l < 0 ? -(int)l : l; if (a > g_peak) g_peak = a;
		a = r < 0 ? -(int)r : r; if (a > g_peak) g_peak = a;
		acc += (double)l * l + (double)r * r;
	}
	g_rms = samples ? sqrt(acc / (2.0 * samples)) : 0.0;
	return h;
}

/* Voices at a spread of pitches and volumes, dry only. */
static uint64_t scen_voices(int samples)
{
	int v;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x90),
		            0x00ff, 0x1fc0, (s16)(0x3000 + v * 0x80), (s16)(0x3fff - v * 0x70));
		start_voice(1, v, 0x20000 + (u32)v * 0x400, (u16)(0x300 + v * 0x77),
		            0x40ff, 0x0fc0, (s16)(0x2000 + v * 0x40), (s16)(0x2fff - v * 0x30));
	}
	Cores[0].DryGate.SndL = Cores[0].DryGate.SndR = -1;
	Cores[1].DryGate.SndL = Cores[1].DryGate.SndR = -1;
	open_ext_path();
	Cores[0].MasterVol.Left.Value = Cores[0].MasterVol.Right.Value = 0x3fff;
	Cores[1].MasterVol.Left.Value = Cores[1].MasterVol.Right.Value = 0x3fff;
	return run(samples);
}

/* The same, with the wet path and reverb enabled -- the reverb buffer is
 * where the resampler and the all-pass chain live. */
static uint64_t scen_reverb(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x500 + v * 0x60),
		            0x10ff, 0x1fc0, 0x3000, 0x3000);
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.SndL = Cores[c].WetGate.SndR = -1;
		Cores[c].FxEnable = 1;
		Cores[c].EffectsStartA = 0x100000 + (u32)c * 0x40000;
		Cores[c].EffectsEndA   = Cores[c].EffectsStartA + 0x3fff0;
		Cores[c].Revb.IN_COEF_L = 0x6000;
		Cores[c].Revb.IN_COEF_R = 0x5800;
		Cores[c].Revb.APF1_SIZE = 0x03c0;
		Cores[c].Revb.APF2_SIZE = 0x02b0;
		Cores[c].Revb.APF1_VOL  = 0x5000;
		Cores[c].Revb.APF2_VOL  = -0x3000;
		Cores[c].Revb.IIR_VOL   = 0x4000;
		Cores[c].Revb.WALL_VOL  = -0x2000;
		Cores[c].Revb.COMB1_VOL = 0x2000;
		Cores[c].Revb.COMB2_VOL = 0x1800;
		Cores[c].Revb.COMB3_VOL = 0x1000;
		Cores[c].Revb.COMB4_VOL = 0x0800;
		Cores[c].Revb.SAME_L_SRC = 0x0800; Cores[c].Revb.SAME_R_SRC = 0x0a00;
		Cores[c].Revb.DIFF_L_SRC = 0x0c00; Cores[c].Revb.DIFF_R_SRC = 0x0e00;
		Cores[c].Revb.SAME_L_DST = 0x1000; Cores[c].Revb.SAME_R_DST = 0x1200;
		Cores[c].Revb.DIFF_L_DST = 0x1400; Cores[c].Revb.DIFF_R_DST = 0x1600;
		Cores[c].Revb.COMB1_L_SRC = 0x1800; Cores[c].Revb.COMB1_R_SRC = 0x1a00;
		Cores[c].Revb.COMB2_L_SRC = 0x1c00; Cores[c].Revb.COMB2_R_SRC = 0x1e00;
		Cores[c].Revb.COMB3_L_SRC = 0x2000; Cores[c].Revb.COMB3_R_SRC = 0x2200;
		Cores[c].Revb.COMB4_L_SRC = 0x2400; Cores[c].Revb.COMB4_R_SRC = 0x2600;
		Cores[c].Revb.APF1_L_DST = 0x2800; Cores[c].Revb.APF1_R_DST = 0x2a00;
		Cores[c].Revb.APF2_L_DST = 0x2c00; Cores[c].Revb.APF2_R_DST = 0x2e00;
		Cores[c].FxVol.Left = Cores[c].FxVol.Right = 0x3fff;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x3fff;
	}
	open_ext_path();
	return run(samples);
}

/* Volume slides running on both the voices and the master, which is the
 * path that has to keep making progress even on a stopped voice. */
static uint64_t scen_slides(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x40),
		            0x00ff, 0x1fc0, 0x2000, 0x2000);
		/* Reg_VOL in slide form: Enable set, with the step, shift and
		 * the decrement/exponential flags varied across the voices so
		 * every branch of the slide update is taken somewhere. */
		Cores[0].Voices[v].Volume.Left.Enable  = 1;
		Cores[0].Voices[v].Volume.Left.Step    = (u16)(v & 3);
		Cores[0].Voices[v].Volume.Left.Shift   = (u16)(8 + (v & 7));
		Cores[0].Voices[v].Volume.Left.Decr    = (u16)((v >> 2) & 1);
		Cores[0].Voices[v].Volume.Left.Exp     = (u16)((v >> 3) & 1);
		Cores[0].Voices[v].Volume.Right.Enable = 1;
		Cores[0].Voices[v].Volume.Right.Step   = (u16)((v + 1) & 3);
		Cores[0].Voices[v].Volume.Right.Shift  = (u16)(9 + (v & 5));
		Cores[0].Voices[v].Volume.Right.Decr   = (u16)((v >> 1) & 1);
		Cores[0].Voices[v].Volume.Right.Exp    = (u16)((v >> 4) & 1);
	}
	/* half the voices stopped, so the stopped-voice slide path runs too */
	for (v = 12; v < 24; v++)
		Cores[0].Voices[v].ADSR.Phase = PHASE_STOPPED;
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].MasterVol.Left.Enable  = 1;
		Cores[c].MasterVol.Left.Step    = 2;
		Cores[c].MasterVol.Left.Shift   = 10;
		Cores[c].MasterVol.Right.Enable = 1;
		Cores[c].MasterVol.Right.Step   = 3;
		Cores[c].MasterVol.Right.Shift  = 11;
		Cores[c].MasterVol.Right.Decr   = 1;
	}
	open_ext_path();
	return run(samples);
}

/* The noise generator and the gates, which are bit operations on the
 * stereo pair rather than arithmetic on it. */
static uint64_t scen_noise_gates(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x50),
		            0x00ff, 0x1fc0, 0x3000, 0x2800);
	Cores[0].NoiseClk = 0x1f;
	Cores[0].Regs.VMIXL = 0x00ffffff;
	Cores[0].Regs.VMIXR = 0x00f0f0f0;
	Cores[0].Regs.VMIXEL = 0x000fff00;
	Cores[0].Regs.VMIXER = 0x00ff00ff;
	for (v = 0; v < 24; v++)
		Cores[0].Voices[v].Modulated = (v & 1);
	Cores[0].VoiceGates[0].DryL = -1;
	for (c = 0; c < 2; c++)
	{
		/* asymmetric gates: left and right must not be interchangeable */
		Cores[c].DryGate.SndL = -1;
		Cores[c].DryGate.SndR = 0;
		Cores[c].DryGate.InpL = 0;
		Cores[c].DryGate.InpR = -1;
		Cores[c].DryGate.ExtL = -1;
		Cores[c].DryGate.ExtR = 0;
		Cores[c].MasterVol.Left.Value  = 0x3fff;
		Cores[c].MasterVol.Right.Value = 0x2000;
	}
	return run(samples);
}

/* The input path: ReadInput takes the ADMA areas of sample RAM through
 * InpVol, which Init leaves at zero -- so without setting it the whole
 * input side multiplies by nothing and cannot be tested. */
static uint64_t scen_input(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 8; v++)
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x400 + v * 0x50),
		            0x00ff, 0x1fc0, 0x2000, 0x2000);
	for (c = 0; c < 2; c++)
	{
		Cores[c].InpVol.Left  = 0x5000;
		Cores[c].InpVol.Right = 0x3800;
		Cores[c].DryGate.InpL = -1;
		Cores[c].DryGate.InpR = -1;
		Cores[c].DryGate.SndL = -1;
		Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.InpL = -1;
		Cores[c].WetGate.InpR = -1;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x3fff;
	}
	open_ext_path();
	return run(samples);
}

/* Driven hard enough to sit on the saturation boundaries. Without this the
 * clamps are never reached -- the other scenarios peak at about half scale
 * -- and a change to how the result is saturated passes unnoticed, which is
 * the first thing a vectorised clamp would get wrong. */
static uint64_t scen_clipping(int samples)
{
	int v, c;
	reset_core();
	for (v = 0; v < 24; v++)
	{
		/* full volume, sustain held at maximum so every voice keeps
		 * contributing rather than decaying out of the clip region */
		start_voice(0, v, 0x1000 + (u32)v * 0x400, (u16)(0x800 + v * 0x40),
		            0x00ff, 0x00c0, 0x7fff, 0x7fff);
		start_voice(1, v, 0x20000 + (u32)v * 0x400, (u16)(0x800 + v * 0x37),
		            0x00ff, 0x00c0, 0x7fff, 0x7fff);
	}
	for (c = 0; c < 2; c++)
	{
		Cores[c].DryGate.SndL = Cores[c].DryGate.SndR = -1;
		Cores[c].WetGate.SndL = Cores[c].WetGate.SndR = -1;
		Cores[c].MasterVol.Left.Value = Cores[c].MasterVol.Right.Value = 0x7fff;
		Cores[c].Regs.VMIXL = Cores[c].Regs.VMIXR = 0x00ffffff;
		Cores[c].Regs.VMIXEL = Cores[c].Regs.VMIXER = 0x00ffffff;
		/* the wet path loud as well, since its clamps are separate */
		Cores[c].FxEnable      = 1;
		Cores[c].EffectsStartA = 0x100000 + (u32)c * 0x40000;
		Cores[c].EffectsEndA   = Cores[c].EffectsStartA + 0x3fff0;
		Cores[c].FxVol.Left    = Cores[c].FxVol.Right = 0x7fff;
		Cores[c].Revb.IN_COEF_L = 0x7fff;
		Cores[c].Revb.IN_COEF_R = -0x7fff;
		Cores[c].Revb.APF1_SIZE = 0x03c0;
		Cores[c].Revb.APF2_SIZE = 0x02b0;
		Cores[c].Revb.APF1_VOL  = 0x7fff;
		Cores[c].Revb.APF2_VOL  = -0x7fff;
		Cores[c].Revb.IIR_VOL   = 0x7fff;
		Cores[c].Revb.WALL_VOL  = -0x7fff;
		Cores[c].Revb.COMB1_VOL = 0x7fff;
		Cores[c].Revb.COMB2_VOL = -0x7fff;
		Cores[c].Revb.COMB3_VOL = 0x7fff;
		Cores[c].Revb.COMB4_VOL = -0x7fff;
		Cores[c].Revb.SAME_L_SRC = 0x0800; Cores[c].Revb.SAME_R_SRC = 0x0a00;
		Cores[c].Revb.DIFF_L_SRC = 0x0c00; Cores[c].Revb.DIFF_R_SRC = 0x0e00;
		Cores[c].Revb.SAME_L_DST = 0x1000; Cores[c].Revb.SAME_R_DST = 0x1200;
		Cores[c].Revb.DIFF_L_DST = 0x1400; Cores[c].Revb.DIFF_R_DST = 0x1600;
		Cores[c].Revb.COMB1_L_SRC = 0x1800; Cores[c].Revb.COMB1_R_SRC = 0x1a00;
		Cores[c].Revb.COMB2_L_SRC = 0x1c00; Cores[c].Revb.COMB2_R_SRC = 0x1e00;
		Cores[c].Revb.COMB3_L_SRC = 0x2000; Cores[c].Revb.COMB3_R_SRC = 0x2200;
		Cores[c].Revb.COMB4_L_SRC = 0x2400; Cores[c].Revb.COMB4_R_SRC = 0x2600;
		Cores[c].Revb.APF1_L_DST = 0x2800; Cores[c].Revb.APF1_R_DST = 0x2a00;
		Cores[c].Revb.APF2_L_DST = 0x2c00; Cores[c].Revb.APF2_R_DST = 0x2e00;
	}
	open_ext_path();
	return run(samples);
}

/* ------------------------------------------------------------------ */

struct Scenario
{
	const char *name;
	uint64_t (*fn)(int);
	uint64_t expect;
};

int main(int argc, char **argv)
{
	/* Samples per scenario. 48000 is a second of audio at the SPU2's
	 * output rate, which is long enough for envelopes to run their
	 * phases and for the reverb buffer to wrap. */
	int samples = argc > 1 ? atoi(argv[1]) : 48000;
	int print   = (argc > 2 && strcmp(argv[2], "--print") == 0);
	int verbose = (argc > 2 && strcmp(argv[2], "--verbose") == 0);
	int i, bad = 0;

	/* Pinned from the tree as it stands, at the default 48000 samples.
	 * A change to the mixer that is meant to be bit-exact leaves every one
	 * of these alone. Re-pin with --print only when the output is meant to
	 * change, and say in the commit why. */
	static Scenario scen[] = {
		{ "voices",      scen_voices,      0xd0822e804df49fceull },
		{ "reverb",      scen_reverb,      0x3a564d9e423bea93ull },
		{ "slides",      scen_slides,      0x2681cc386a8e3c29ull },
		{ "noise+gates", scen_noise_gates, 0xe9c06d20f41a8280ull },
		{ "clipping",    scen_clipping,    0xa889e93e69cb254bull },
		{ "input",       scen_input,       0xedc162f6bc1ee9f5ull },
	};

	fill_sample_ram();

	for (i = 0; i < (int)(sizeof(scen) / sizeof(scen[0])); i++)
	{
		uint64_t h;
		fill_sample_ram();
		h = scen[i].fn(samples);

		/* A scenario that emits nothing would hash very consistently and
		 * test nothing, so each one has to be audible before its hash
		 * means anything. */
		if (strcmp(scen[i].name, "clipping") == 0 &&
		    (g_hi_rail < 64 || g_lo_rail < 64))
		{
			printf("  %-12s NOT SATURATING: rails +%ld/-%ld -- the clamps are untested\n",
			       scen[i].name, g_hi_rail, g_lo_rail);
			bad++;
			continue;
		}
		if (g_nonzero < samples / 4 || g_peak < 256)
		{
			printf("  %-12s SILENT: %ld/%d non-zero, peak %d -- scenario is not mixing\n",
			       scen[i].name, g_nonzero, samples, g_peak);
			bad++;
			continue;
		}
		if (print)
			printf("  { \"%s\", %016llx },\n", scen[i].name,
			       (unsigned long long)h);
		else if (scen[i].expect == 0)
			printf("  %-12s %016llx  (unpinned)\n", scen[i].name,
			       (unsigned long long)h);
		else if (h != scen[i].expect)
		{
			printf("  %-12s %016llx  EXPECTED %016llx\n", scen[i].name,
			       (unsigned long long)h, (unsigned long long)scen[i].expect);
			bad++;
		}
		else
			printf("  %-12s %016llx  ok\n", scen[i].name,
			       (unsigned long long)h);

		if (!print && verbose)
			printf("               %ld/%d non-zero, peak %d, rms %.0f, rails +%ld/-%ld\n",
			       g_nonzero, samples, g_peak, g_rms, g_hi_rail, g_lo_rail);
	}

	if (!print)
		printf("%s: SPU2 PCM, %d scenarios, %d mismatches\n",
		       bad ? "FAIL" : "PASS",
		       (int)(sizeof(scen) / sizeof(scen[0])), bad);
	return bad != 0;
}
