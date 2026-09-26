#!/bin/sh
# The recompiler switches are seen where they are tested.
#
# recswitch : every unit under pcsx2/ that tests a *_RECOMPILE switch from
#             Config.h in a preprocessor conditional includes Config.h
#             above that test. A unit that does not reads the switch as
#             undefined and compiles its instruction group to interpreter
#             calls without a warning.
#
# The negative check runs the tool on a copy of iFPU.cpp without its
# Config.h include, which must fail.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}

$CC -std=c89 -pedantic -Wall -Wextra -O2 -o "$DIR/recswitch" "$DIR/recswitch.c"

SOURCES=$(find "$ROOT/pcsx2" -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.inl' \) \
	! -path '*/GS/*' ! -name 'Config.h' | sort)
"$DIR/recswitch" "$ROOT/pcsx2/Config.h" $SOURCES

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
grep -v '#include "../Config.h"' "$ROOT/pcsx2/x86/iFPU.cpp" > "$TMP/iFPU.cpp"
if "$DIR/recswitch" "$ROOT/pcsx2/Config.h" "$TMP/iFPU.cpp" > "$TMP/out" 2>&1; then
	echo "negative: iFPU.cpp without Config.h passed"
	exit 1
fi
echo "negative: iFPU.cpp without Config.h is caught"
