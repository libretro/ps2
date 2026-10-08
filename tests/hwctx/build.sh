#!/bin/sh
# Which hardware contexts the core asks the frontend for.
#
# Usage: sh tests/hwctx/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== no OpenGL ES context, software renderer as the last resort =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/hwctx_ctxaudit" "$DIR/ctxaudit.c"
"$DIR/hwctx_ctxaudit" "$ROOT/libretro/main.cpp" "$ROOT/pcsx2/GS/GS.cpp"
