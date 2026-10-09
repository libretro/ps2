#!/bin/sh
# The VIF dynarec's block hash: lookups, resets, and the shared empty
# sentinel. Plain and ASan+UBSan+LSan; MSYS2 MINGW64 builds and runs the
# plain one.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
CC=${CC:-cc}
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) WINDOWS=1; EXE=.exe ;;
  *)                    WINDOWS=0; EXE="" ;;
esac

build() { # output, extra flags
	"$CC" -std=c89 -pedantic -Wall -Wno-long-long $2 -I "$ROOT/pcsx2" -I "$LC/include" \
		-o "$DIR/$1$EXE" "$DIR/main.c" "$LC/memmap/memalign.c"
}

build vifhash "-O2 -g"
"$DIR/vifhash$EXE"
if [ "$WINDOWS" != "1" ]; then
	build vifhash_asan "-O1 -g -fsanitize=address,undefined"
	"$DIR/vifhash_asan"
fi
