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
