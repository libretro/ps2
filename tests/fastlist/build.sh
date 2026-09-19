#!/bin/sh
# FastList (pcsx2/GS/Renderers/Common/GSFastList.h): the intrusive list the
# texture cache keeps one of per GS page. It allocates on the first
# insertion rather than at construction, so the empty case is the one
# worth pinning - an unallocated list must size, compare, and iterate like
# any other empty one.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
LC="$ROOT/libretro/libretro-common"
INC="-I $ROOT -I $ROOT/pcsx2 -I $ROOT/common -I $ROOT/common/include -I $LC/include"
SRC="$DIR/main.cpp $LC/memmap/memalign.c"

${CXX:-c++} -std=c++17 -O2 -g -w $SANFLAGS $INC -o "$DIR/fastlist_test" $SRC
"$DIR/fastlist_test"
