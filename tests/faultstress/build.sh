#!/bin/sh
# Concurrent page-fault dispatch stress: plain (+ TSan where available).
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-gcc}

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;;
  *)                    WINDOWS=0 ;;
esac

if [ "$WINDOWS" = "1" ]; then
  # MinGW: Win32 threads + vectored exception handler; no TSan on Windows.
  "$CC" -O2 -g -std=gnu99 -Wall -o "$DIR/fault_test.exe" "$DIR/main.c"
  echo "built: $DIR/fault_test.exe"
  echo "note: ThreadSanitizer is unavailable on Windows - the counter"
  echo "      mismatch check is your detector here; run with more threads"
  echo "      and more faults for confidence, e.g. fault_test 4 500000"
else
  "$CC" -O2 -g -std=gnu99 -Wall -o "$DIR/fault_test" "$DIR/main.c" -lpthread
  "$CC" -O1 -g -std=gnu99 -Wall -fsanitize=thread \
    -o "$DIR/fault_test_tsan" "$DIR/main.c" -lpthread
  echo "built: $DIR/fault_test $DIR/fault_test_tsan"
fi
