#!/bin/sh
# The GSdx Vulkan renderer's context negotiation against a frontend that
# speaks version 2 (device made through its wrapper), one whose wrapper
# fails the first, explicit-GPU attempt, and one that speaks version 1
# only. Each boots, runs frames and tears down; see main.c.
#
# Runs the built core: pcsx2_libretro.so/.dll/.dylib at the top of the
# tree, or LRPS2_CORE. Without one, or without a Vulkan device (lavapipe
# will do), the harness is still compiled and the run is skipped. Set
# LRPS2_BIOS to a real BIOS image to also require frames as images.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) CORE="$ROOT/pcsx2_libretro.dll";   LIBS="" ;;
	Darwin)               CORE="$ROOT/pcsx2_libretro.dylib"; LIBS="" ;;
	*)                    CORE="$ROOT/pcsx2_libretro.so";    LIBS="-ldl" ;;
esac
CORE=${LRPS2_CORE:-$CORE}

# C99, not C89: the Vulkan headers are.
${CC:-cc} -std=c99 -pedantic -Wall -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$DIR/vknegotiate" "$DIR/main.c" $LIBS
echo "built: $DIR/vknegotiate"

if [ ! -f "$CORE" ]; then
	echo "skip: no core at $CORE (build it, or set LRPS2_CORE)"
	exit 0
fi

for mode in v2 v2retry v1; do
	SCRATCH=$(mktemp -d)
	if ! timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" $mode; then
		rm -rf "$SCRATCH"
		echo "  FAIL: Vulkan negotiation, $mode frontend"
		exit 1
	fi
	rm -rf "$SCRATCH"
done
