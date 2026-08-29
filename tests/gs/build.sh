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

# Micro-benchmark: is the vector accept/cull decision worth replacing on this
# host? Prints ns/prim for the shipped kernel and for the scalar-outcode form
# GV-3 proposes, after checking the two agree.
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_vertex_bench" "$DIR/gs_vertex_bench.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_vertex_bench"

# Would fusing FindMinMax into the kick pay off on this host? Compares the
# legacy index walk against accumulate-at-kick, with the sticky-NaN work x86
# would need included on the fused side.
g++ -O2 -std=c++17 -msse4.1 -o "$DIR/gs_fmm_bench" "$DIR/gs_fmm_bench.cpp" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include"
"$DIR/gs_fmm_bench"
