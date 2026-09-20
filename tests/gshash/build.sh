#!/bin/sh
# The GS hash table (GS/Renderers/HW/GSHashTable.c), built as C89 like
# the core does, and run.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SRC="$DIR/main.c $ROOT/pcsx2/GS/Renderers/HW/GSHashTable.c"

${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-I "$ROOT/pcsx2/GS/Renderers/HW" -o "$DIR/gshash_test" $SRC
"$DIR/gshash_test"
