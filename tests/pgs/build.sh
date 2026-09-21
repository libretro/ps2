#!/bin/sh
# paraLLEl-GS vertex-kick harness. Mirrors tests/gs/build.sh.
#
# pgs_vertex_oracle : the SIMD position build must be byte-identical to the
#                     current scalar body across randomized register state.
# pgs_queue_equiv   : the ring-buffer vertex queue must deliver the same
#                     (position, attribute) triples to drawing_kick_append as
#                     the shift queue, for every topology.
# pgs_kick_bench    : ns/vertex for each candidate. Run under BOTH g++ and
#                     clang++ -- the hand-SIMD attribute build is a large gcc
#                     win and a large clang regression, so a single-compiler
#                     number decides nothing here.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
PGS="$ROOT/pcsx2/GS/parallel-gs"
INC="-I$PGS/Granite/math -I$PGS/gs"

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	echo "=== $CXX ==="
	$CXX -O2 -std=c++17 -msse4.1 $INC -o "$DIR/pgs_vertex_oracle" "$DIR/pgs_vertex_oracle.cpp"
	"$DIR/pgs_vertex_oracle"
	$CXX -O2 -std=c++17 -msse4.1 $INC -o "$DIR/pgs_queue_equiv" "$DIR/pgs_queue_equiv.cpp"
	"$DIR/pgs_queue_equiv"
	$CXX -O2 -std=c++17 -msse4.1 $INC -o "$DIR/pgs_kick_bench" "$DIR/pgs_kick_bench.cpp"
	"$DIR/pgs_kick_bench" 2500 25
done
