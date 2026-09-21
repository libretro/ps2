#!/bin/sh
# paraLLEl-GS vertex-kick harness. Mirrors tests/gs/build.sh.
#
# pgs_c89_check     : the kernel header must compile as strict C89.
# pgs_vertex_oracle : the C89 kernels must write bytes identical to the
#                     muglm field-by-field bodies they replace.
# pgs_queue_equiv   : a ring-buffer vertex queue must deliver the same
#                     (position, attribute) triples to drawing_kick_append
#                     as the shift queue, for every topology.
# pgs_prim_record_equiv : the cached per-primitive record must equal one
#                     rebuilt from the registers every primitive -- a register
#                     writer that forgets its dirty bit shows up here.
# pgs_parallelogram_equiv : the scalar rewrite of the two gs_util predicates
#                     must agree with the muglm form, float path included --
#                     NaN, +/-0 and infinity all appear in the inputs.
# pgs_kick_bench    : ns/vertex per candidate.
#
# Everything runs under BOTH g++ and clang++. The two disagree about which
# field they compile well -- gcc rebuilds the packed UV in six instructions
# where clang uses two, clang splits the ST pair into two moves where gcc
# merges them -- so a number from one compiler decides nothing here.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
PGS="$ROOT/pcsx2/GS/parallel-gs"
INC="-I$DIR -I$PGS/Granite/math -I$PGS/gs"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	echo "=== $CC -std=c89 -pedantic ==="
	$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC \
	    -c "$DIR/pgs_c89_check.c" -o "$DIR/pgs_c89_check.o"
	echo "    clean"
done
rm -f "$DIR/pgs_c89_check.o"

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	echo "=== $CXX ==="
	for t in pgs_vertex_oracle pgs_queue_equiv pgs_prim_record_equiv pgs_parallelogram_equiv pgs_kick_bench; do
		$CXX -O2 -std=c++17 -msse4.1 $INC -o "$DIR/$t" "$DIR/$t.cpp"
	done
	"$DIR/pgs_vertex_oracle"
	"$DIR/pgs_queue_equiv"
	"$DIR/pgs_prim_record_equiv"
	"$DIR/pgs_parallelogram_equiv"
	"$DIR/pgs_kick_bench" 2500 25
done
