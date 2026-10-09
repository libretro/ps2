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
# tests/swdraw/main.c is built here too, for a BIOS image of its own.
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

# A GIF packet that is still being sent when the state is saved
# (tests/swdraw's gif_split case, its BIOS image written by that harness):
# the state saved straight after its load is again the one loaded, on
# both renderers.
SCRATCH=$(mktemp -d)
${CC:-cc} -std=c89 -pedantic -Wall -Wno-long-long -O2 \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$SCRATCH/swdraw" "$ROOT/tests/swdraw/main.c" $LIBS
"$SCRATCH/swdraw" --bios "$SCRATCH/gif_split.bin" gif_split
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/gif_split.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: savestate mid-packet, $renderer"
		exit 1
	fi
done

# A copy out of a block, then an upload over that block in the same
# packet: the copy reads what was there before the upload, and the state
# holds the block it went to. paraLLEl-GS only: the Vulkan renderer's
# state holds local memory, which a copy between targets does not reach.
"$SCRATCH/swdraw" --bios "$SCRATCH/copy_then_upload.bin" copy_then_upload
if ! VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/copy_then_upload.bin" VN_RENDERER=paraLLEl-GS \
		timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
	rm -rf "$SCRATCH"
	echo "  FAIL: copy then upload, paraLLEl-GS"
	exit 1
fi

# Local to host transfers into RAM, the second of 4-bit pixels: RAM holds
# the pixels, on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/readback_t4hh.bin" readback_t4hh
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=55555555 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/readback_t4hh.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: 4-bit readback, $renderer"
		exit 1
	fi
done

# Destination alpha a clear wrote and a draw wrote, read back and read by
# a blend: the same alpha either way, on both renderers.
"$SCRATCH/swdraw" --bios "$SCRATCH/dest_alpha.bin" dest_alpha
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=5a000000,0000005a VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/dest_alpha.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: destination alpha, $renderer"
		exit 1
	fi
done

# 20000 copies in one packet with nothing to end the submission: the
# batches of copies stay within what one holds.
"$SCRATCH/swdraw" --bios "$SCRATCH/many_copies.bin" many_copies
for renderer in Vulkan paraLLEl-GS; do
	if ! VN_EXPECT_RUN=78563412 VN_IDLE_BIOS=1 LRPS2_BIOS="$SCRATCH/many_copies.bin" VN_RENDERER=$renderer \
			timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" v2; then
		rm -rf "$SCRATCH"
		echo "  FAIL: many copies, $renderer"
		exit 1
	fi
done
rm -rf "$SCRATCH"

# A driver that runs out of device memory while the renderer comes up, at
# each allocation the renderer makes before it is up: the device is
# refused cleanly or the renderer comes up and runs. vkshim.c stands in for
# the loader the core opens, so this needs the real one by path; Linux only.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*|Darwin) echo "skip: alloc modes are Linux only"; exit 0 ;; esac
REAL=$(ldconfig -p 2>/dev/null | awk '/libvulkan\.so\.1 /{print $NF; exit}')
if [ -z "$REAL" ]; then
	echo "skip: alloc modes need libvulkan.so.1"
	exit 0
fi
SHIM=$(mktemp -d)
${CC:-cc} -std=c99 -Wall -O2 -shared -fPIC \
	-isystem "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$SHIM/libvulkan.so.1" "$DIR/vkshim.c" -ldl
for mode in v2alloc v1alloc; do
	for granted in 0 1 2 3 4 5 6 7; do
		SCRATCH=$(mktemp -d)
		if ! LD_LIBRARY_PATH="$SHIM" VN_REAL_VULKAN="$REAL" VN_ALLOC_OK=$granted VN_FRAMES=10 \
				timeout 300 "$DIR/vknegotiate" "$CORE" "$SCRATCH" $mode; then
			rm -rf "$SCRATCH" "$SHIM"
			echo "  FAIL: Vulkan negotiation, $mode, $granted allocations granted"
			exit 1
		fi
		rm -rf "$SCRATCH"
	done
done
rm -rf "$SHIM"
