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

echo "== game database entries vs the real parser =="
LC="$ROOT/libretro/libretro-common"
${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
	-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
	-I "$LC/include" \
	-o "$DIR/patch_dbload" "$DIR/dbload.cpp" \
	"$ROOT/pcsx2/GameDatabase.cpp" "$ROOT/pcsx2/Pcsx2Config.cpp" \
	"$ROOT/common/StringUtil.cpp" "$ROOT/common/Threads.cpp" \
	"$ROOT/common/Semaphore.cpp" \
	"$LC/formats/yaml/ryaml.c" "$LC/rthreads/rthreads.c" \
	"$LC/file/file_path.c" "$LC/string/rstrtod.c" "$LC/string/stdstring.c" \
	"$LC/compat/fopen_utf8.c" "$LC/compat/compat_strl.c" \
	"$LC/encodings/encoding_utf.c" "$LC/time/rtime.c" -lpthread
"$DIR/patch_dbload" "$ROOT/bin/resources/GameIndex.yaml"
