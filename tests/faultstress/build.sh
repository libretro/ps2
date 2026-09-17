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

  # And the same stress against the linked implementation, which is
  # libretro-common's faulthandler rather than a model of it.
  ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
  LC="$ROOT/libretro/libretro-common"
  "$CC" -O2 -g -std=gnu99 -Wall -DHAVE_THREADS -I "$LC/include" \
    -o "$DIR/fault_linked" "$DIR/linked.c" \
    "$LC/faulthandler/faulthandler.c" "$LC/memmap/memmap.c" "$LC/rthreads/rthreads.c" \
    -lpthread -lrt
  "$DIR/fault_linked" 3 20000
  # TSan sees the dispatch; its own SEGV handler must not take the
  # faults first, which is how a fastmem core runs.
  "$CC" -O1 -g -std=gnu99 -Wall -fsanitize=thread -DHAVE_THREADS -I "$LC/include" \
    -o "$DIR/fault_linked_tsan" "$DIR/linked.c" \
    "$LC/faulthandler/faulthandler.c" "$LC/memmap/memmap.c" "$LC/rthreads/rthreads.c" \
    -lpthread -lrt
  TSAN_OPTIONS=handle_segv=0 "$DIR/fault_linked_tsan" 3 2000
fi
