#!/bin/sh
# Float exactness audit + JIT transcription proof for the exact add/sub
# stage. Both must print zero diffs for the emission to be trusted.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
gcc -O2 -std=c89 -o "$DIR/fp_audit" "$DIR/fp_audit.c" "$ROOT/pcsx2/ps2float.c" \
    -I"$ROOT/pcsx2" -I"$ROOT/libretro/libretro-common/include" -lm
g++ -O2 -std=c++17 -no-pie -o "$DIR/jit_stage" "$DIR/jit_stage.cpp" "$ROOT/pcsx2/ps2float.c" \
    -I"$ROOT" -I"$ROOT/common" -I"$ROOT/common/include" -I"$ROOT/pcsx2" \
    -I"$ROOT/3rdparty" -I"$ROOT/3rdparty/include" \
    -I"$ROOT/libretro/libretro-common/include" -lm
"$DIR/fp_audit"
"$DIR/jit_stage"
