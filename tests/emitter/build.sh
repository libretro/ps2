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

echo "== branch and call reach =="
ALIGNSAN=""
if echo 'int main(void){return 0;}' | ${CC:-cc} -x c -fsanitize=alignment \
	-fno-sanitize-recover=alignment -o /dev/null - 2>/dev/null; then
	ALIGNSAN="-fsanitize=alignment -fno-sanitize-recover=alignment"
fi
${CC:-cc} -std=c89 -pedantic -Wno-long-long -Wall -Wno-unused-function -O1 $SANFLAGS $ALIGNSAN \
	-I "$ROOT" -o "$DIR/emitter_reach" "$DIR/reach.c"
"$DIR/emitter_reach"
