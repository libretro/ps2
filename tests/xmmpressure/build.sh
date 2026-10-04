#!/bin/sh
# Every VU upper op, compiled by the shipping recompiler with the VU
# accuracy options on, fits the fifteen allocatable XMM registers -- in
# micro mode on VU0 and VU1 and as a COP2 macro op through the EE
# recompiler. An op that does not makes the core abort at the allocator.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one the harness is still compiled and the
# run is skipped. No BIOS or content is needed; the harness writes a
# synthetic BIOS image into a scratch system directory.
#
# The harness is not built with SANITIZER: what it checks happens inside
# the core, which is built separately.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/xmmpressure" "$DIR/main.c" $LIBS
echo "built: $DIR/xmmpressure"

case "$(uname -m)" in
	x86_64|amd64|AMD64) ;;
	*) echo "skip: the x86-64 recompiler is what this checks"; exit 0 ;;
esac
if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
"$DIR/xmmpressure" "$CORE" "$SCRATCH"
