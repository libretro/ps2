#!/bin/sh
# SANITIZER=... must match whatever the core objects were built with:
# this harness LINKS pcsx2/IPU/*.o, so a plain build against sanitized
# objects fails with undefined __asan_*/__ubsan_* references.
#   make SANITIZER=address,undefined && SANITIZER=address,undefined sh tests/ipu/build.sh
# IPU differential-oracle harness.  Links the repo's already-built IPU
# objects (build the core first) against a small stub layer and drives
# them through IPUCMD_WRITE / IPUProcessInterrupt / the real FIFOs.
# Usage:  sh tests/ipu/build.sh   (from the repo root)
#         ./tests/ipu/ipu_test out.txt && diff tests/ipu/golden_baseline.txt out.txt
set -e
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"
INC="-I pcsx2 -I . -I common -I 3rdparty/include -I 3rdparty/cpuinfo/include -I libretro/libretro-common/include -I libretro"
g++ -O2 -msse4.1 -std=c++17 $SANFLAGS $INC -c tests/ipu/main.cpp  -o tests/ipu/main.o
g++ -O2 -msse4.1 -std=c++17 $SANFLAGS $INC -c tests/ipu/stubs.cpp -o tests/ipu/stubs.o
CPUOBJS=$(find 3rdparty/cpuinfo -name "*.o")
g++ $SANFLAGS -o tests/ipu/ipu_test tests/ipu/main.o tests/ipu/stubs.o \
  pcsx2/IPU/IPU.o pcsx2/IPU/IPU_Fifo.o \
  pcsx2/IPU/IPU_MultiISA.sse4.o pcsx2/IPU/IPU_MultiISA.avx.o pcsx2/IPU/IPU_MultiISA.avx2.o \
  pcsx2/IPU/IPUdither.sse4.o pcsx2/IPU/IPUdither.avx.o pcsx2/IPU/IPUdither.avx2.o \
  pcsx2/IPU/yuv2rgb.sse4.o pcsx2/IPU/yuv2rgb.avx.o pcsx2/IPU/yuv2rgb.avx2.o \
  $CPUOBJS
echo built: tests/ipu/ipu_test
