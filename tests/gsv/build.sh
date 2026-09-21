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
#   store     : the per-vertex store, byte-for-byte, both when the source is
#               cold and when its upper half was written an instruction
#               earlier, which is how VertexKick reaches it.
#   numbers   : ns/vertex, gs_vertex beside GSVertex.
#
# Everything runs under BOTH g++ and clang++, at each ISA level. The two
# disagree about which form they compile well, so a number from one compiler
# decides nothing here; a form is only kept when neither regresses.
#
# With a cross toolchain present (aarch64-linux-gnu-gcc, qemu-aarch64) the
# NEON bodies run too -- pass --neon.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include -I$ROOT/pcsx2/GS/Renderers/Common"
N=${N:-4000000}
R=${R:-9}

TMP=${TMPDIR:-/tmp}/gsv.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# gs_vertex.h is header-only, so C89 conformance is checked by compiling a
# C translation unit that includes it and calls every entry point.
echo "=== strict C89 ==="
printf '#include "gs_vertex.h"\nint main(void){ union gs_vertex a, b; gs_vec4i r;\n b.w[0]=1; gs_vertex_store(&a,&b);\n r=gs_vertex_xy(&a); r=gs_vertex_z(&a); r=gs_vertex_uv(&a);\n r=gs_vertex_rgba(&a); r=gs_vertex_fog(&a); (void)r;\n (void)gs_vertex_st(&a); (void)gs_vertex_q(&a); return 0; }\n' > "$TMP/c89.c"
gcc -std=c89 -pedantic -Wno-long-long -Wall -Wextra -O2 -msse4.1 $INC -c "$TMP/c89.c" -o "$TMP/c89.o"
echo "gcc -std=c89 -pedantic: clean"

for CC in gcc clang; do
	# libretro.h declares bool, which clang reports as a C99 extension on
	# every declaration in a C89 build. Not this file's warnings.
	case $CC in
		gcc)   CXX=g++;     Q= ;;
		clang) CXX=clang++; Q=-Wno-c99-extensions ;;
	esac
	command -v $CC >/dev/null 2>&1 || { echo "skipping $CC, not installed"; continue; }
	for ISA in "-msse2" "-msse4.1" "-mavx" "-mavx2"; do
		echo
		echo "=== $CXX $ISA ==="
		$CC  -O2 -std=c89 -pedantic -Wall -Wextra $Q $ISA $INC -c "$TMP/c89.c" -o "$TMP/c89.o"
		$CXX -O2 -std=c++17 $ISA $INC "$DIR/gs_vertex_equiv.cpp" -o "$TMP/t"
		"$TMP/t" "$N" "$R"
	done
done

if [ "$1" = "--neon" ]; then
	echo
	echo "=== aarch64 / NEON ==="
	aarch64-linux-gnu-gcc -O2 -std=c89 -pedantic -Wall -Wextra $INC -c "$TMP/c89.c" -o "$TMP/c64.o"
	aarch64-linux-gnu-g++ -O2 -std=c++17 $INC "$DIR/gs_vertex_equiv.cpp" -o "$TMP/t64"
	# qemu times nothing meaningful -- it is here for the correctness lanes.
	qemu-aarch64 -L /usr/aarch64-linux-gnu "$TMP/t64" 200000 3
fi
