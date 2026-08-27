#!/bin/sh
# MMI divide checks.
#
# Two things, cheapest first:
#
#   divsign.sh   reads the recompiler sources and checks that every CDQ is
#                followed by an IDIV and every zeroing of EDX by a DIV.
#                Catches the PDIVW/PDIVBW bug by inspection, at review
#                speed, without building anything.
#
#   divcheck.c   emits the guarded divide sequences through the same
#                macros the recompiler uses, executes them, and compares
#                against pcsx2/MMI.cpp. Catches the same bug by running
#                it: a sign-extended dividend fed to an unsigned divide
#                either returns the wrong quotient or raises #DE, and the
#                second of those takes the process down with SIGFPE.
#
# Usage: sh tests/mmi/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== divide signedness (static) =="
sh "$DIR/divsign.sh"

echo "== divide execution (emit and run) =="
"$CC" -O1 -g -Wall -Wno-unused-function $SANFLAGS \
	-I "$ROOT" -I "$ROOT/common" -I "$ROOT/pcsx2" \
	-o "$DIR/mmi_divcheck" "$DIR/divcheck.c"
"$DIR/mmi_divcheck"
