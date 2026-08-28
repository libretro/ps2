#!/bin/sh
# IOP (R3000A) core against ps2autotests console captures.
#
# Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/iop/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== IOP multiply and divide vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwmuldiv" "$DIR/hwmuldiv.c"
	"$DIR/iop_hwmuldiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP ALU vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/alu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwalu" "$DIR/hwalu.c"
	"$DIR/iop_hwalu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi
