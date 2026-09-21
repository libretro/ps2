#!/bin/sh
# IPU inverse DCT harness.
#
# IDCT_Block's column pass has two spellings -- pmaddwd from SSE4.1 up,
# scalar below it -- and only one of them is compiled into any given build.
# Running every tier against one pinned hash is what holds them together:
# -msse2 takes the scalar pass, -msse4.1 and -mavx2 take the vector one, so
# a hash that differs between them is the finding. The blocks also go
# through a double-precision IDCT, which catches a mistake that was in both
# spellings all along.
#
#   ./idct_build.sh            check against the pinned hashes
#   ./idct_build.sh --print    print hashes to re-pin, when output should change
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
N=${N:-20000}
ISAS=${ISAS:-"-msse2 -msse4.1 -mavx2"}

TMP=${TMPDIR:-/tmp}/ipuidct.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || { echo "skipping $CXX"; continue; }
	case $CXX in g++) CC=gcc ;; clang++) CC=clang ;; esac
	for ISA in $ISAS; do
		echo
		echo "=== $CXX $ISA ==="
		$CC -O2 -std=gnu89 -Wall -Wdeclaration-after-statement $ISA $INC \
		    -c "$ROOT/pcsx2/IPU/ipu_idct.c" -o "$TMP/idct.o"
		$CXX -O2 -std=c++17 $ISA $INC -c "$DIR/idct_hash.cpp" \
		     -o "$TMP/hash.o"
		$CXX -O2 "$TMP/hash.o" "$TMP/idct.o" -lm -o "$TMP/idct_hash"
		"$TMP/idct_hash" "$N" "$1"
	done
done

# aarch64 runs its own NEON column pass and byte clamp. Holding it to the
# same hashes as the SSE4.1 build is what says the two vector spellings
# agree; -msse2 above is where the scalar fallback still runs.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo
	echo "=== aarch64 (NEON column pass and clamp) ==="
	aarch64-linux-gnu-gcc -O2 -std=gnu89 -Wall $INC \
	     -c "$ROOT/pcsx2/IPU/ipu_idct.c" -o "$TMP/idct64.o"
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC -c "$DIR/idct_hash.cpp" \
	     -o "$TMP/hash64.o"
	aarch64-linux-gnu-g++ -O2 -static "$TMP/hash64.o" "$TMP/idct64.o" -lm \
	     -o "$TMP/idct_hash64"
	qemu-aarch64 "$TMP/idct_hash64" "$N" "$1"
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi

# mingw builds the kernel as C, where __forceinline carries a storage class
# that static collides with. The tree spells it __fi for exactly that
# reason; this is what keeps it spelled that way.
MINGW_FORCEINLINE='extern __inline__ __attribute__((__always_inline__,__gnu_inline__))'
echo
echo "=== mingw C decoration ==="
gcc -O2 -std=gnu89 -msse4.1 $INC -D__MINGW32__ \
    "-D__forceinline=$MINGW_FORCEINLINE" \
    -fsyntax-only "$ROOT/pcsx2/IPU/ipu_idct.c" 2>&1 | grep -i " error" && exit 1
echo "ok"
