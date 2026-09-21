#!/bin/sh
# IPU bitstream window harness.
#
# The readers serve a run of bits from an arbitrary bit position by loading
# a machine word and shifting. The model in bitstream_hash.cpp walks the
# window a bit at a time instead, so a masking or endianness mistake shows
# up rather than being reproduced, and every bit position in 0..127 is
# swept at every width the contract allows.
#
#   ./bitstream_build.sh            check against the pinned hash
#   ./bitstream_build.sh --print    print the hash to re-pin
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
N=${N:-2000}

TMP=${TMPDIR:-/tmp}/ipubs.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac
	for ISA in -msse2 -msse4.1 -mavx2; do
		echo
		echo "=== $CXX $ISA ==="
		# The readers are header-only so they inline into the
		# decoder; this compiles the header as C on its own first,
		# since the harness itself is C++ and would not catch a
		# C89 slip in it.
		echo '#include "IPU/ipu_bitstream.h"' > "$TMP/conly.c"
		echo 'int main(void){return 0;}' >> "$TMP/conly.c"
		$CC -O2 -std=gnu89 -pedantic -Wno-long-long -Wall \
		    -Wdeclaration-after-statement $ISA $INC \
		    -c "$TMP/conly.c" -o "$TMP/conly.o"
		$CXX -O2 -std=c++17 $ISA $INC -c "$DIR/bitstream_hash.cpp" \
		     -o "$TMP/hash.o"
		$CXX -O2 "$TMP/hash.o" -o "$TMP/bs_hash"
		"$TMP/bs_hash" "$N" "$1"
	done
done

# Big-endian is the interesting one here: the readers byteswap, so a host
# that is already big-endian has to reach the same bits by doing nothing.
# s390x is the cross target that is actually packaged.
if command -v s390x-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-s390x >/dev/null 2>&1; then
	echo
	echo "=== s390x (big-endian host) ==="
	s390x-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/bitstream_hash.cpp" \
	     -o "$TMP/hash390.o"
	s390x-linux-gnu-g++ -O2 -static "$TMP/hash390.o" -o "$TMP/bs_hash390"
	qemu-s390x "$TMP/bs_hash390" "$N" "$1"
else
	echo
	echo "skipping s390x lane (no cross toolchain or qemu)"
fi

if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 ==="
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/bitstream_hash.cpp" \
	     -o "$TMP/hash64.o"
	aarch64-linux-gnu-g++ -O2 -static "$TMP/hash64.o" -o "$TMP/bs_hash64"
	qemu-aarch64 "$TMP/bs_hash64" "$N" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi
