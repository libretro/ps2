#!/bin/sh
# GS draws on the software renderer, through the built core on a
# synthetic BIOS image: each case's packet is drawn while 30 frames run,
# and the process must survive and keep presenting. Against a core built
# with SANITIZER=address,undefined (LRPS2_CORE=...), an out-of-bounds
# access in the renderer fails the case too.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one the harness is still compiled and the
# run is skipped.
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
	-o "$DIR/swdraw" "$DIR/main.c" $LIBS
echo "built: $DIR/swdraw"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
failed=0
for c in aa1_small aa1_triangle aa1_triangle_2x aa1_line aa1_line_2x mip_tw8 mip_tw11 mip_tw11_2x display_large display_large_2x; do
	if ! timeout 300 "$DIR/swdraw" "$CORE" "$SCRATCH" "$c"; then
		echo "  FAIL: $c"
		failed=1
	fi
done
exit $failed
