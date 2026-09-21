#!/bin/sh
# gs_vertex harness. Mirrors tests/pgs/build.sh.
#
# gs_vertex_equiv runs four lanes:
#
#   layout    : the record must keep the exact size, alignment and field
#               offsets GSVertex has. Nothing else here can catch a field
#               that moves -- the accessors address the record by vector
#               lane, not by field name, so a moved field is not a compile
#               error anywhere, it is wrong geometry.
#   accessors : all seven field accessors must be byte-identical to the
#               GSVertex ones over millions of records, edge patterns
#               included. GSVertex has an arm64 backend of its own, so this
#               lane compares NEON against NEON there rather than against
#               scalar.
#   batch     : every batch backend the build contains and the host can run,
#               not only the one dispatch picks -- gs_vertex_set_backend()
#               pins each in turn -- against the scalar contract, over every
#               length from 0 to 33 so each body's scalar tail is exercised.
#   numbers   : ns/vertex, gs_vertex beside GSVertex.
#
# Everything runs under BOTH g++ and clang++, at each ISA level. The two
# disagree about which form they compile well, so a number from one compiler
# decides nothing here; a form is only kept when neither regresses.
#
# With a cross toolchain present (aarch64-linux-gnu-gcc, qemu-aarch64) the
# NEON backend runs too -- pass --neon.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include -I$ROOT/pcsx2/GS/Renderers/Common"
SRC="$ROOT/pcsx2/GS/Renderers/Common/gs_vertex.c"
LRC="$ROOT/libretro/libretro-common"
N=${N:-4000000}
R=${R:-9}

# libretro-common units gs_vertex.c needs. Built as the project builds them.
build_support()
{
	$1 -O2 -std=gnu99 -I"$LRC/include" -c "$LRC/features/features_cpu.c" -o "$2/features_cpu.o"
	$1 -O2 -std=gnu99 -I"$LRC/include" -c "$LRC/compat/compat_strl.c"     -o "$2/compat_strl.o"
}

TMP=${TMPDIR:-/tmp}/gsv.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

echo "=== strict C89 ==="
gcc -std=c89 -pedantic -Wno-long-long -Wall -Wextra -O2 -msse4.1 $INC -c "$SRC" -o "$TMP/c89.o"
echo "gcc -std=c89 -pedantic: clean"

for CC in gcc clang; do
	# libretro.h declares bool, which clang reports as a C99 extension on
	# every declaration in a C89 build. Not this file's warnings.
	case $CC in
		gcc)   CXX=g++;     Q= ;;
		clang) CXX=clang++; Q=-Wno-c99-extensions ;;
	esac
	command -v $CC >/dev/null 2>&1 || { echo "skipping $CC, not installed"; continue; }
	build_support $CC "$TMP"
	for ISA in "-msse2" "-msse4.1" "-mavx" "-mavx2"; do
		echo
		echo "=== $CXX $ISA ==="
		$CC  -O2 -std=c89 -pedantic -Wall -Wextra $Q $ISA $INC -c "$SRC" -o "$TMP/v.o"
		$CXX -O2 -std=c++17 $ISA $INC "$DIR/gs_vertex_equiv.cpp" \
		     "$TMP/v.o" "$TMP/features_cpu.o" "$TMP/compat_strl.o" -o "$TMP/t"
		"$TMP/t" "$N" "$R"
	done
done

if [ "$1" = "--neon" ]; then
	echo
	echo "=== aarch64 / NEON ==="
	build_support aarch64-linux-gnu-gcc "$TMP"
	aarch64-linux-gnu-gcc -O2 -std=c89 -pedantic -Wall -Wextra $INC -c "$SRC" -o "$TMP/v64.o"
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC "$DIR/gs_vertex_equiv.cpp" \
	     "$TMP/v64.o" "$TMP/features_cpu.o" "$TMP/compat_strl.o" -o "$TMP/t64"
	# qemu times nothing meaningful -- it is here for the correctness lanes.
	qemu-aarch64 -L /usr/aarch64-linux-gnu "$TMP/t64" 200000 3
fi
