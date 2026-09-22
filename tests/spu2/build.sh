#!/bin/sh
# SPU2 PCM harness.
#
# spu2_pcm_hash links the real SPU2 -- Mixer, spu2sys, ADSR, Reverb,
# ReverbResample, RegTable, ReadInput, Dma -- against stubs for the IOP
# interrupt and DMA callbacks and the audio sink, none of which take part in
# mixing. It then drives voices, envelopes, volume slides, the noise
# generator, reverb and the gates, and hashes every stereo sample Mix()
# emits.
#
# The hashes are pinned. A change to the mixer meant to be bit-exact leaves
# all six alone; anything else has altered what a game hears. Re-pin with
#
#   ./build.sh --print
#
# only when the output is meant to change, and say why in the commit.
#
# Run it under both compilers and every ISA tier: the point is the PCM, and
# all of them must agree on it, so a hash that moves between two builds is
# itself a finding. The reverb resampler carries a 128-bit and a 256-bit
# path and they are meant to be bit-identical, which is what the -mavx2
# lane checks; -msse2 checks the fallbacks gs_vector spells out for the
# tier the GS classes cannot reach.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/SPU2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
# Every SPU2 unit builds as C, including the shim that faces the emulator.
UNITS_C="Mixer spu2sys ADSR Reverb ReverbResample RegTable ReadInput Dma spu2freeze spu2"
UNITS_CXX=""
N=${N:-48000}
ISAS=${ISAS:-"-msse2 -msse4.1 -mavx2"}

# mingw spells __forceinline with a storage class in C but not in C++, so a
# `static ... __forceinline` that builds here fails there. The project's own
# __fi is empty on mingw, which is what these sources use; this lane compiles
# them the way mingw's C preprocessor sees them so the difference cannot come
# back unnoticed.
MINGW_FORCEINLINE='extern __inline__ __attribute__((__always_inline__,__gnu_inline__))'

TMP=${TMPDIR:-/tmp}/spu2.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

echo
echo "=== mingw C decoration ==="
for u in $UNITS_C; do
	gcc -O2 -std=gnu99 -msse4.1 $INC -D__MINGW32__ \
	    "-D__forceinline=$MINGW_FORCEINLINE" \
	    -fsyntax-only "$ROOT/pcsx2/SPU2/$u.c" 2>&1 | grep ": error:" && exit 1
done
echo "  ok, $UNITS_C"

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac
	for ISA in $ISAS; do
		echo
		echo "=== $CXX $ISA ==="
		for u in $UNITS_C; do
			$CC -O2 -std=gnu89 -Wdeclaration-after-statement $ISA $INC \
			    -c "$ROOT/pcsx2/SPU2/$u.c" -o "$TMP/$u.o"
		done
		$CXX -O2 -std=c++17 $ISA $INC -c "$DIR/spu2_pcm_hash.cpp" -o "$TMP/hash.o"
		$CXX -O2 -std=c++17 "$TMP/hash.o" \
		     $(for u in $UNITS_C $UNITS_CXX; do echo "$TMP/$u.o"; done) \
		     -o "$TMP/spu2_pcm_hash"
		"$TMP/spu2_pcm_hash" "$N" "$1"
	done
done

# Every scenario must hash the same alone and after every other one, or
# the pins above describe an order, not a scenario. Quadratic, so once.
echo
echo "=== scenario isolation ==="
"$TMP/spu2_pcm_hash" "$N" --isolation

# aarch64 runs the NEON spellings gs_vector carries for adds16, hadds16 and
# mul16hrs, which the reverb resampler leans on for every tap. Nothing had
# ever executed them -- the x86 lanes cannot reach them and a compile check
# does not say the arithmetic agrees. Same PCM or it is a finding.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (NEON reverb resampler) ==="
	for u in $UNITS_C; do
		aarch64-linux-gnu-gcc -O2 -std=gnu89 -Wdeclaration-after-statement \
		    $INC -c "$ROOT/pcsx2/SPU2/$u.c" -o "$TMP/a_$u.o"
	done
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/spu2_pcm_hash.cpp" \
	     -o "$TMP/a_hash.o"
	aarch64-linux-gnu-g++ -O2 -static "$TMP/a_hash.o" \
	     $(for u in $UNITS_C $UNITS_CXX; do echo "$TMP/a_$u.o"; done) \
	     -o "$TMP/spu2_pcm_hash64"
	qemu-aarch64 "$TMP/spu2_pcm_hash64" "$N" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi
