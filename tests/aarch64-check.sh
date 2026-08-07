#!/bin/sh
# aarch64 cross-build + qemu-user run of the standalone test suites.
# Usage: sh tests/aarch64-check.sh   (from anywhere)
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/.." && pwd)
CC=${AARCH64_CC:-aarch64-linux-gnu-gcc}
RUN=${AARCH64_RUN:-qemu-aarch64-static}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

command -v "$CC"  >/dev/null || { echo "missing $CC (apt install gcc-aarch64-linux-gnu)"; exit 2; }
command -v "$RUN" >/dev/null || { echo "missing $RUN (apt install qemu-user-static)"; exit 2; }

echo "== retro_spsc =="
"$CC" -O2 -std=c99 -Wall -static -I "$ROOT/libretro/libretro-common/include" \
  -o "$OUT/spsc" "$ROOT/tests/spsc/main.c" \
  "$ROOT/libretro/libretro-common/queues/retro_spsc.c" -lpthread
"$RUN" "$OUT/spsc"

echo "== fault dispatch =="
"$CC" -O2 -g -std=gnu99 -Wall -static -o "$OUT/fault" "$ROOT/tests/faultstress/main.c" -lpthread
"$RUN" "$OUT/fault" 4 20000 | tail -1

echo "== barrier codegen (must be non-zero on both sides) =="
OBJDUMP=${AARCH64_OBJDUMP:-aarch64-linux-gnu-objdump}
for f in retro_spsc_write retro_spsc_read; do
  n=$("$OBJDUMP" -d "$OUT/spsc" | awk "/<$f>:/,/^\$/" | grep -cE 'ldar|stlr|dmb' || true)
  echo "  $f: $n acquire/release instructions"
  [ "$n" -ge 2 ] || { echo "  FAIL: $f has no real barriers - relaxed regression?"; exit 1; }
done
echo "aarch64 check OK"
