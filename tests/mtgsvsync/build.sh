#!/bin/sh
# Where in a vsync the EE is let go, and what the scanout reads after.
#
# MTGS::MainLoop consumes the EE's vsync packet, copies the privileged
# registers, goes idle - which releases the EE from WaitGS - and only
# then scans the frame out and hands it to the frontend. main.c runs that
# order on the real work eventcount with a producer thread, and the two
# orders it replaced are built as well and must fail: one holds the EE
# through every present, the other scans out the next frame's registers.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
build() { # rule, output, extra flags
	${CC:-cc} -O1 -g -std=gnu99 -Wall $3 -DHAVE_THREADS -DRULE=$1 -I "$LC/include" -I "$ROOT/pcsx2" -I "$ROOT" \
		-o "$DIR/$2" "$DIR/main.c" $SRC -lpthread
}

build 0 mtgsvsync "$SANFLAGS"
"$DIR/mtgsvsync"

for old in 1 2; do
	build $old mtgsvsync_rule$old ""
	if "$DIR/mtgsvsync_rule$old" > /dev/null 2>&1; then
		echo "  FAIL: rule $old passed; the reproducer has stopped reproducing"
		exit 1
	fi
done
echo "  ok: both orders this replaced still fail (1 holds the EE through the present, 2 scans out the next frame's registers)"

build 0 mtgsvsync_tsan "-fsanitize=thread -Wno-tsan"
"$DIR/mtgsvsync_tsan"
