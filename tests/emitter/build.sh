#!/bin/sh
# The C emitter macros against GNU as.
#
# The old oracle.cpp compared the C++ emitter against these macros; the
# C++ side has been removed, so this compares them against an external
# assembler instead. Needs `as` and `objdump`, and skips without them.
#
# Usage: sh tests/emitter/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== C emitter macros vs GNU as =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS \
	-I "$ROOT" -I "$ROOT/common" -I "$ROOT/common/include" \
	-I "$ROOT/pcsx2" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/emitter_hwas" "$DIR/hwas.cpp"
"$DIR/emitter_hwas"

# The other five files in this directory -- oracle, exhaustive,
# shim_oracle, newbind, sse_exhaustive and switch_equiv -- all reach the
# removed C++ emitter, whether through reference/x86emitter_shim.h or
# directly via the x86Emitter namespace and xADD/xRegister32. None of them
# build, and each is a rewrite rather than a fix, so none is run here.
