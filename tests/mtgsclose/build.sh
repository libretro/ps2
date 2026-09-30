#!/bin/sh
# The frame the EE had ready, when the frontend takes the context away.
#
# The EE runs a frame ahead of the scanout, so a drain between two
# retro_runs finds that frame's vsync in the ring. pcsx2/MTGSOwner.h
# (mtgs_vsync_presents) says what the drain does with it: present it,
# so a savestate does not swallow a frame - except while the frontend is
# inside context_destroy, where the present is a video_refresh made
# under the frontend's own context lock. RetroArch up to 1.22 never
# returns from that call: closing content hung it (issue #171).
#
# main.c runs the rule on the real work eventcount with a producer thread
# and a frontend that holds a lock across its context_destroy. The two
# rules either side of it are built as well and must fail: one presents
# from inside the teardown, the other loses the frame at every savestate.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
SANFLAGS="${SANFLAGS:-}"
SRC="$LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c $LC/rthreads/retro_asym_eventcount.c $LC/rthreads/retro_procbarrier.c"
build() { # rule, output, extra flags
	${CC:-cc} -O1 -g -std=gnu99 -Wall $3 -DHAVE_THREADS -DRULE=$1 -I "$LC/include" -I "$ROOT/pcsx2" \
		-o "$DIR/$2" "$DIR/main.c" $SRC -lpthread
}

build 0 mtgsclose "$SANFLAGS"
"$DIR/mtgsclose"

for old in 1 2; do
	build $old mtgsclose_rule$old ""
	if "$DIR/mtgsclose_rule$old" > /dev/null 2>&1; then
		echo "  FAIL: rule $old passed; the reproducer has stopped reproducing"
		exit 1
	fi
done
echo "  ok: both rules either side of it still fail (1 presents inside the teardown, 2 loses the savestate's frame)"

# The ring counters and the pause flags are what the two threads share.
build 0 mtgsclose_tsan "-fsanitize=thread -Wno-tsan"
"$DIR/mtgsclose_tsan"
