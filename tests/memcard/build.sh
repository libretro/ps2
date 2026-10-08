#!/bin/sh
# Memory card protocol checks.
#
# Usage: sh tests/memcard/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== memory card terminator commands =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/memcard_termaudit" "$DIR/termaudit.c"
"$DIR/memcard_termaudit" "$ROOT/pcsx2/MemoryCardProtocol.cpp"
