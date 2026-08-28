#!/bin/sh
# VU semantics checks against ps2autotests console captures.
#
# hwclip scores _vuCLIP against tests/vu/upper/clip.expected. The expected
# values are inlined, so it needs no capture checkout.
#
# hwint scores the integer lower ops against tests/vu/lower/integer.expected
# and reads that file directly, so point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
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

echo "== VU integer ops vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/vu/lower/integer.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/vu_hwint" "$DIR/hwint.c"
	"$DIR/vu_hwint" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi
