#!/bin/sh
# Oracle suite for the GS packed-vertex parse kernels. Must print zero
# mismatches for the kernels in GS/GSVertexKick.h to be trusted.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_vertex_oracle" "$DIR/gs_vertex_oracle.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_vertex_oracle"
