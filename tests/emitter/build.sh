#!/bin/sh
# x86 emitter differential oracle.
#
# Emits every case twice at the SAME address -- once through the C++ emitter
# in common/emitter, once through the C89 macro core in
# common/emitter/c89emit.h -- and compares the bytes. The same address matters
# because RIP-relative displacements depend on the emit cursor.
#
# The C++ emitter is the reference by definition: any divergence is a bug in
# the C89 core. Exit status is non-zero if anything diverges.
#
# Build the core first (the harness links common/emitter/*.o), then:
#   sh tests/emitter/build.sh && ./tests/emitter/emitter_oracle
#
# SANITIZER=... must match whatever the core objects were built with.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I $ROOT -I $ROOT/common -I $ROOT/common/include -I $ROOT/pcsx2 \
 -I $ROOT/3rdparty -I $ROOT/3rdparty/include \
 -I $ROOT/libretro/libretro-common/include -I $ROOT/common/emitter"
g++ -O1 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/oracle.cpp" -o "$DIR/oracle.o"
g++ -O1 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/shim_oracle.cpp" -o "$DIR/shim_oracle.o"
# -no-pie so the emit buffer can be MAP_FIXED at a known address, which keeps
# the RIP-relative displacements reproducible between the two emitters.
g++ $SANFLAGS -no-pie -o "$DIR/emitter_oracle" "$DIR/oracle.o" \
  "$ROOT"/common/emitter/avx.o "$ROOT"/common/emitter/groups.o \
  "$ROOT"/common/emitter/jmp.o "$ROOT"/common/emitter/legacy.o \
  "$ROOT"/common/emitter/legacy_sse.o "$ROOT"/common/emitter/movs.o \
  "$ROOT"/common/emitter/simd.o "$ROOT"/common/emitter/x86emitter.o
EMOBJS="$ROOT/common/emitter/avx.o $ROOT/common/emitter/groups.o \
  $ROOT/common/emitter/jmp.o $ROOT/common/emitter/legacy.o \
  $ROOT/common/emitter/legacy_sse.o $ROOT/common/emitter/movs.o \
  $ROOT/common/emitter/simd.o $ROOT/common/emitter/x86emitter.o"
g++ $SANFLAGS -no-pie -o "$DIR/shim_oracle" "$DIR/shim_oracle.o" $EMOBJS
echo "built: $DIR/emitter_oracle"
echo "built: $DIR/shim_oracle"
