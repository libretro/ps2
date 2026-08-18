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
# Build the core first (the harness links tests/emitter/reference/*.o), then:
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
  "$ROOT"/tests/emitter/reference/avx.o "$ROOT"/tests/emitter/reference/groups.o \
  "$ROOT"/tests/emitter/reference/jmp.o "$ROOT"/tests/emitter/reference/legacy.o \
  "$ROOT"/tests/emitter/reference/legacy_sse.o "$ROOT"/tests/emitter/reference/movs.o \
  "$ROOT"/tests/emitter/reference/simd.o "$ROOT"/tests/emitter/reference/x86emitter.o
EMOBJS="$ROOT/tests/emitter/reference/avx.o $ROOT/tests/emitter/reference/groups.o \
  $ROOT/tests/emitter/reference/jmp.o $ROOT/tests/emitter/reference/legacy.o \
  $ROOT/tests/emitter/reference/legacy_sse.o $ROOT/tests/emitter/reference/movs.o \
  $ROOT/tests/emitter/reference/simd.o $ROOT/tests/emitter/reference/x86emitter.o"
g++ $SANFLAGS -no-pie -o "$DIR/shim_oracle" "$DIR/shim_oracle.o" $EMOBJS
# Switchover equivalence: the same driver built both ways, bytes diffed.
# The reference build links the emitter objects; the switched build gets the
# group1 objects from the header instead, so it needs the rest of them still.
g++ -O2 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/switch_equiv.cpp" -o "$DIR/equiv_ref.o"
g++ $SANFLAGS -no-pie -o "$DIR/equiv_ref" "$DIR/equiv_ref.o" $EMOBJS
g++ -O2 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG -DPCSX2_C89_EMITTER \
    $SANFLAGS $INC -c "$DIR/switch_equiv.cpp" -o "$DIR/equiv_sw.o"
g++ $SANFLAGS -no-pie -o "$DIR/equiv_sw" "$DIR/equiv_sw.o" $EMOBJS

g++ -O0 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/exhaustive.cpp" -o "$DIR/exhaustive.o"
g++ $SANFLAGS -no-pie -o "$DIR/exhaustive" "$DIR/exhaustive.o" $EMOBJS
g++ -O0 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/sse_exhaustive.cpp" -o "$DIR/sse_exhaustive.o"
g++ $SANFLAGS -no-pie -o "$DIR/sse_exhaustive" "$DIR/sse_exhaustive.o" $EMOBJS
g++ -O0 -std=c++17 -fPIC -fno-semantic-interposition -DNDEBUG $SANFLAGS $INC \
    -c "$DIR/newbind.cpp" -o "$DIR/newbind.o"
g++ $SANFLAGS -no-pie -o "$DIR/newbind" "$DIR/newbind.o" $EMOBJS

echo "built: $DIR/emitter_oracle"
echo "built: $DIR/shim_oracle"
echo "built: $DIR/exhaustive"
echo "built: $DIR/sse_exhaustive"
echo "built: $DIR/newbind"
echo "built: $DIR/equiv_ref and $DIR/equiv_sw"
echo "  compare with: diff <(setarch $(uname -m) -R $DIR/equiv_ref) \\"
echo "                     <(setarch $(uname -m) -R $DIR/equiv_sw)"
