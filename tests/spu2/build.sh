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
# Run it under both compilers: the point is the PCM, and the two must agree
# on it, so a hash that moves between them is itself a finding.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/SPU2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
UNITS="Mixer spu2sys ADSR Reverb ReverbResample RegTable ReadInput Dma"
N=${N:-48000}

TMP=${TMPDIR:-/tmp}/spu2.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	echo
	echo "=== $CXX ==="
	for u in $UNITS; do
		$CXX -O2 -std=c++17 -msse4.1 $INC -c "$ROOT/pcsx2/SPU2/$u.cpp" -o "$TMP/$u.o"
	done
	$CXX -O2 -std=c++17 -msse4.1 $INC -c "$DIR/spu2_pcm_hash.cpp" -o "$TMP/hash.o"
	$CXX -O2 -std=c++17 "$TMP/hash.o" "$TMP"/*.o -o "$TMP/spu2_pcm_hash" 2>/dev/null ||
		$CXX -O2 -std=c++17 "$TMP/hash.o" $(for u in $UNITS; do echo "$TMP/$u.o"; done) -o "$TMP/spu2_pcm_hash"
	"$TMP/spu2_pcm_hash" "$N" "$1"
done
