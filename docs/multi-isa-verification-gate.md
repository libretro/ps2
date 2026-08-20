# Multi-ISA enablement: verification gate

Commits 1-4 added an opt-in multi-ISA build (`ENABLE_MULTI_ISA`, default OFF).
This is the checklist that must pass before flipping the default to ON
(the final commit). Until every item passes, ship with the switch OFF.

The hard requirement throughout: **no regression for CPUs at or below the
current SSE4.1 floor.** Item 2 is the direct test of that and is the
non-negotiable gate.

---

## 0. Build the multi-ISA configuration

    make clean
    make ENABLE_MULTI_ISA=1 -j$(nproc)

Expect 45 extra objects (15 sources x 3 tiers): GSRasterizer.sse4.o,
GSRasterizer.avx.o, GSRasterizer.avx2.o, ... If the link fails with multiple
definitions, the ODR ordering or a missing `CURRENT_ISA::` qualifier on a
global is the cause (see MultiISA.h notes).

Sanity-check the object set and order:

    # 45 tier objects, grouped sse4 -> avx -> avx2
    make -p ENABLE_MULTI_ISA=1 2>/dev/null | grep '^MULTI_ISA_OBJECTS :=' \
      | tr ' ' '\n' | grep -oE '\.(sse4|avx|avx2)\.o' | uniq -c
    # expect: 15 sse4, then 15 avx, then 15 avx2

Confirm OFF is still byte-identical (no tier objects, no shared define):

    make -n ENABLE_MULTI_ISA=0 2>/dev/null | grep -cE '\.(sse4|avx|avx2)\.o'   # 0
    make -n ENABLE_MULTI_ISA=0 2>/dev/null | grep -c MULTI_ISA_SHARED_COMPILATION # 0

---

## 1. Verify all three tiers actually got compiled in

The point of the change is that AVX2 code EXISTS in the binary. Confirm each
tier's namespace symbols are present:

    nm -C ps2_libretro.so | grep -E 'isa_(sse4|avx|avx2)::' | \
      sed -E 's/.*(isa_(sse4|avx|avx2)).*/\1/' | sort | uniq -c
    # expect nonzero counts for all three

And that AVX2 instructions are actually present in the avx2 namespace code
(spot check a known-vectorized function):

    objdump -d ps2_libretro.so | awk '/isa_avx2.*GSRasterizer/,/ret/' | \
      grep -ciE 'vpadd|vmovdqu|vpmull|ymm'   # expect > 0

---

## 2. SSE4 SAFETY GATE (the hard requirement) -- MUST PASS

Goal: prove that on a CPU with no AVX/AVX2, the core selects the sse4 path and
NEVER executes an AVX/AVX2 instruction anywhere (including in shared inline
COMDAT symbols).

### 2a. Static check -- no AVX leaked into the sse4 path or shared code

The isa_sse4 functions and all shared (non-tier) objects must contain zero
VEX-encoded (AVX) instructions. Disassemble and scan:

    objdump -d ps2_libretro.so > /tmp/dis.txt
    # AVX instructions are VEX/EVEX encoded; their mnemonics start with 'v'
    # (vmovaps, vpaddd, vzeroupper, ...). In the isa_sse4 namespace there
    # should be NONE.
    awk '/isa_sse4::/{f=1} /isa_avx[2]?::/{f=0} f' /tmp/dis.txt | \
      grep -iE '\b(v[a-z]+ps|v[a-z]+pd|vp[a-z]+|vzeroupper|ymm)\b' | head
    # expect: no output

If anything prints, a shared inline compiled under AVX flags was linked into
the sse4 path -> ODR ordering failed -> do NOT ship. (Commits ordered objects
sse4-first specifically to prevent this; a hit here means re-check OBJECTS
order and that no shared TU is being built with AVX flags.)

### 2b. Dynamic check with Intel SDE (Nehalem = SSE4.2, no AVX)

SDE faults on any instruction the emulated CPU lacks, so any stray AVX on the
sse4 path becomes a hard error at the moment of execution.

    # -nhm emulates Nehalem (no AVX). Run the core through a frontend, or a
    # minimal load+run harness, against a GS-heavy title on the SW or HW
    # (NON-parallel-gs) renderer.
    sde64 -nhm -- retroarch -L ps2_libretro.so /path/to/game.iso

    # Pass: boots, runs, no #UD / "tried to execute AVX..." fault from SDE.
    # Fail: SDE reports an unsupported-instruction fault -> AVX on sse4 path.

Also confirm the selection logic chose sse4 under -nhm (add a one-off log in
MULTI_ISA_SELECT or check the reported CPU features): CpuHasAVX() must be false
and the sse4 namespace functions must be the ones called.

### 2c. (Ideal) real hardware

If a physical pre-AVX machine is available (Core 2 / first-gen i-series,
SSE4.1 only), the SDE result should be confirmed there: boot + run a title on
the fallback renderer with no crash.

---

## 3. Cross-toolchain build matrix

Build ENABLE_MULTI_ISA=1 on each target and smoke-test:

    - [ ] gcc   (Linux)            -- build, load, run a title
    - [ ] clang (Linux/macOS)      -- build, load, run
    - [ ] MinGW (Windows cross)    -- build; watch for -Wa,-mbig-obj needs now
                                      that GS objects are tripled (object/
                                      section count up); link clean
    - [ ] MSVC2017 (windows_msvc) -- build with /arch:AVX /arch:AVX2 tiers;
                                      link clean; run

MinGW note: the Makefile already special-cases big-obj for some GS sources;
3x as many GS objects may surface new cases. If assembler complains about too
many sections, add -Wa,-mbig-obj to the tier compile flags for MinGW.

---

## 4. Performance validation (decides whether ON-by-default is worth it)

The win only applies to the SW/HW fallback renderers; parallel-gs bypasses all
multi-ISA GS code. So measure on a NON-parallel-gs renderer:

    - Pick a GS-heavy title, software (or HW) renderer.
    - Measure: ENABLE_MULTI_ISA=0 (SSE4 only) vs =1 (AVX2 selected on an AVX2
      host). Real-hardware frame timing / internal GS timing, not synthetic.
    - Expect improvement in GS swizzle / rasterizer / IPU paths on AVX2 hosts.

If the measured win is negligible (e.g. because realistic users run
parallel-gs), the correct decision may be to KEEP the switch off-by-default
and document it as an opt-in tuning flag rather than flip it.

---

## 5. Flip the default (only after 0-4 pass)

One-line change:

    -ENABLE_MULTI_ISA ?= 0
    +ENABLE_MULTI_ISA ?= 1

Commit message should record: which CPUs were tested for the SSE4 gate (2b/2c),
the toolchains built (3), and the measured win (4). Keep the ability to set
`ENABLE_MULTI_ISA=0` for anyone who needs the single-ISA build.

---

## Rollback

The switch makes rollback trivial: if a regression surfaces post-flip, set the
default back to 0 (or build with ENABLE_MULTI_ISA=0). No source changes are
needed -- the single-ISA SSE4.1 path is always present and unchanged.
