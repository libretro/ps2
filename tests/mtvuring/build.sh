#!/bin/sh
# MTVU ring pointer protocol under concurrency.
#
# This test FAILS against the protocol as MTVU.cpp has it. That is what
# it is for: MTVU.cpp's WaitOnSize carries a FIXME about a queue-pointer
# bug that corrupts the SotC intro, papered over with a 4KB safety net,
# and this is that bug without needing the game.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
N=${MTVU_RING_PACKETS:-200000}
${CC:-cc} -O2 -g -std=gnu99 -Wall -DHAVE_THREADS -I "$LC/include" \
	-o "$DIR/mtvu_ring" "$DIR/main.c" "$LC/rthreads/rthreads.c" -lpthread
echo "built: $DIR/mtvu_ring"
"$DIR/mtvu_ring" "$N" || echo "(expected: the protocol is known broken; see main.c)"
