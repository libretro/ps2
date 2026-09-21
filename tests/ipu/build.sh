#!/bin/sh
# IPU kernel harness.
#
# yuv2rgb and ipu_dither each have a SIMD path and a scalar path that are
# meant to agree. This links the real kernels against a scalar model of
# what they are supposed to compute -- written from the operation, not
# lifted from either path -- and compares block for block.
#
# Every tier, both compilers: SSE2 and SSE4.1 go different ways through
# the dither's pack, and AVX2 changes what the compiler emits under both.
# A hash that moves between two of these builds is itself a finding.
#
#   ./build.sh            check against the pinned hashes
#   ./build.sh --print    print hashes to re-pin, when output should change
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
# Both kernels build as C; the harness itself is C++, which is also what
# the emulator links them from.
UNITS="IPU/yuv2rgb IPU/IPUdither IPU/ipu_csc"
N=${N:-20000}
ISAS=${ISAS:-"-msse2 -msse4.1 -mavx2"}

TMP=${TMPDIR:-/tmp}/ipu.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac
	for ISA in $ISAS; do
		echo
		echo "=== $CXX $ISA ==="
		for u in $UNITS; do
			$CC -O2 -std=gnu89 -Wdeclaration-after-statement $ISA $INC \
			    -c "$ROOT/pcsx2/$u.c" -o "$TMP/$(basename $u).o"
		done
		$CXX -O2 -std=c++17 $ISA $INC -c "$DIR/ipu_kernel_hash.cpp" \
		     -o "$TMP/hash.o"
		$CXX -O2 "$TMP/hash.o" "$TMP"/yuv2rgb.o "$TMP"/IPUdither.o "$TMP"/ipu_csc.o \
		     -o "$TMP/ipu_kernel_hash"
		"$TMP/ipu_kernel_hash" "$N" "$1"
	done
done

# The scalar branch of both kernels is dead code on x86 -- the #if takes the
# SSE path always -- so it can drift from the SIMD path unnoticed. aarch64 is
# where it is live (yuv2rgb has a NEON path, the dither does not), so run it
# there too and hold it to the same hashes. That is what proves the two
# spellings still agree.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (scalar dither, NEON yuv2rgb) ==="
	for u in $UNITS; do
		aarch64-linux-gnu-gcc -O2 -std=gnu89 $INC -c "$ROOT/pcsx2/$u.c" \
		     -o "$TMP/$(basename $u).o"
	done
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/ipu_kernel_hash.cpp" \
	     -o "$TMP/hash.o"
	aarch64-linux-gnu-g++ -O2 -static "$TMP/hash.o" "$TMP"/yuv2rgb.o \
	     "$TMP"/IPUdither.o "$TMP"/ipu_csc.o -o "$TMP/ipu_kernel_hash64"
	qemu-aarch64 "$TMP/ipu_kernel_hash64" "$N" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi
