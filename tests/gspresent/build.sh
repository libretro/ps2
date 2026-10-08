#!/bin/sh
# The retire list GSDevice keeps present textures on, built as C89 and run.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS -I "$ROOT/pcsx2" \
	-o "$DIR/present_lifetime" "$DIR/present_lifetime.c" \
	"$ROOT/pcsx2/GS/Renderers/Common/GSRetireRing.c"
"$DIR/present_lifetime"
