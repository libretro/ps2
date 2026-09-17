#!/bin/sh
# retro_spsc stress: plain (+ TSan where available).
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-gcc}
INC="-I $ROOT/libretro/libretro-common/include"
SRC="$DIR/main.c $ROOT/libretro/libretro-common/queues/retro_spsc.c"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
  *)                    WINDOWS=0 ;;
esac

if [ "$WINDOWS" = "1" ]; then
  "$CC" -O2 -std=c99 -Wall $INC -o "$DIR/spsc_test.exe" $SRC
  echo "built: $DIR/spsc_test.exe   (no TSan on Windows)"
else
  "$CC" -O2 -std=c99 -Wall $INC -o "$DIR/spsc_test" $SRC -lpthread
  "$CC" -O1 -g -std=c99 -Wall -fsanitize=thread $INC \
    -o "$DIR/spsc_test_tsan" $SRC -lpthread
  echo "built: $DIR/spsc_test $DIR/spsc_test_tsan"
fi

# The worker-notify handshake in pcsx2/WorkEventCount.h: two eventcounts.
# Plain and TSan, like the queue above: the two threads share nothing
# but the primitive, which is exactly what TSan is for.
if [ "$WINDOWS" != "1" ]; then
  LC="$ROOT/libretro/libretro-common"
  WS_SRC="$DIR/worksema.cpp \
    $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c \
    $LC/rthreads/retro_procbarrier.c $LC/rthreads/retro_asym_eventcount.c"
  WS_INC="-I $ROOT -I $ROOT/common -I $ROOT/common/include -I $LC/include"
  ${CXX:-c++} -std=c++17 -O1 -g -w -D_GNU_SOURCE -DHAVE_THREADS $WS_INC \
    -o "$DIR/spsc_worksema" $WS_SRC -lpthread
  "$DIR/spsc_worksema"
  ${CXX:-c++} -std=c++17 -O1 -g -w -fsanitize=thread -D_GNU_SOURCE -DHAVE_THREADS $WS_INC \
    -o "$DIR/spsc_worksema_tsan" $WS_SRC -lpthread
  "$DIR/spsc_worksema_tsan"
fi
