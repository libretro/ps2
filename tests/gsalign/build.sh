#!/bin/sh
# Alignment of the buffers inside pooled texture cache objects.
#
# The GS compares and copies with aligned SSE loads - GSVector4i's
# compare64 and update cast a pointer and index it - so a buffer they run
# over has to be 16-byte aligned. Moving the palette's CLUT into the
# object put it at offset eight and every comparison became a general
# protection fault: an access violation whose address Windows reports as
# -1, in PaletteKeyEqual, in every game at startup.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"

${CXX:-c++} -std=c++17 -O2 -g -w -msse2 $SANFLAGS \
	-I "$ROOT/pcsx2/GS/Renderers/HW" -I "$LC/include" \
	-o "$DIR/gsalign_test" \
	"$DIR/main.cpp" "$ROOT/pcsx2/GS/Renderers/HW/GSObjectPool.c" "$LC/memmap/memalign.c"
"$DIR/gsalign_test"
