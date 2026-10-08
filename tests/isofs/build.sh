#!/bin/sh
# IsoFS path lookup on the real IsoFS.cpp and IsoFile.cpp, against a
# disc built in memory. MSYS2 MINGW64 builds and runs it as is.
# Runnable from ANY directory - paths resolve relative to this script.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
P="$ROOT/pcsx2"
CC=${CC:-cc}
CXX=${CXX:-c++}
SANFLAGS="${SANFLAGS:-}"
INC="-I $ROOT -I $P -I $ROOT/common -I $ROOT/common/include -I $ROOT/3rdparty/include \
  -I $ROOT/libretro/libretro-common/include"

case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
  *)                    EXE="" ;;
esac

OBJ="$DIR/.obj"
rm -rf "$OBJ" && mkdir -p "$OBJ"
"$CC" -std=c89 -pedantic -Wall -O1 -g $SANFLAGS $INC -c "$DIR/main.c" -o "$OBJ/main.o"
"$CXX" -std=c++17 -w -O1 -g $SANFLAGS $INC -D__LIBRETRO__ -o "$DIR/isofs_test$EXE" \
  "$DIR/glue.cpp" "$P/CDVD/IsoFS/IsoFS.cpp" "$P/CDVD/IsoFS/IsoFile.cpp" "$OBJ/main.o"
rm -rf "$OBJ"
"$DIR/isofs_test$EXE"
