#!/bin/sh
# Every patch line in the game database, through the real parser.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== game database patch lines vs the real parser =="
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$ROOT/libretro/libretro-common/include" \
	-o "$DIR/patch_dbparse" "$DIR/dbparse.cpp" \
	"$ROOT/pcsx2/Patch.cpp" "$ROOT/common/StringUtil.cpp"
"$DIR/patch_dbparse" "$ROOT/bin/resources/GameIndex.yaml"
