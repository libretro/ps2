#!/bin/sh
# EE integer core against ps2autotests console captures.
#
# hwmuldiv scores DIV, DIVU, MULT and MULTU against
# tests/cpu/ee/muldiv.expected. Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/ee/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== EE integer muldiv vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwmuldiv" "$DIR/hwmuldiv.c"
	"$DIR/ee_hwmuldiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== EE ALU three-register ops vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/ee/alu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/ee_hwalu" "$DIR/hwalu.c"
	"$DIR/ee_hwalu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi
