#!/bin/sh
# EE FPU arithmetic against ps2autotests console captures.
#
# hwfpu links pcsx2/ps2float.c, so it exercises the real implementation the
# FPU interpreter calls rather than a copy of it. Point PS2AUTOTESTS at a
# checkout:  git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/fpu/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== EE FPU vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee_fpu"
if [ -d "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" -I "$ROOT/common" \
		-o "$DIR/fpu_hwfpu" "$DIR/hwfpu.c" "$ROOT/pcsx2/ps2float.c"
	"$DIR/fpu_hwfpu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi
