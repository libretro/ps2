#!/bin/sh
# VU semantics checks against ps2autotests console captures.
#
# hwclip scores _vuCLIP against tests/vu/upper/clip.expected. The expected
# values are inlined, so unlike tests/mmi this needs no capture checkout.
#
# Usage: sh tests/vu/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== VU CLIP vs console =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwclip" "$DIR/hwclip.c"
"$DIR/vu_hwclip"
