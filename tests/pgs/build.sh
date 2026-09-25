#!/bin/sh
# paraLLEl-GS vertex-kick harness. Mirrors tests/gs/build.sh.
#
# pgs_layout_check  : the shared structs must keep the exact sizes and
#                     offsets the precompiled shader bank was built against.
# pgs_c89_check     : the kernel header must compile as strict C89.
# pgs_field_scanout : the high-res scanout factors of field-rendered games
#                     and the sample layers the circuit shader reads for them.
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
# Everything above runs on x86 and again on aarch64 under qemu, because the
# pair kernels branch on the host ISA and the NEON arm is the one the core's
# main target takes.
#
# Everything runs under BOTH g++ and clang++. The two disagree about which
# field they compile well -- gcc rebuilds the packed UV in six instructions
# where clang uses two, clang splits the ST pair into two moves where gcc
# merges them -- so a number from one compiler decides nothing here.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
PGS="$ROOT/pcsx2/GS/parallel-gs"
INC="-I$DIR -I$PGS/gs"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	echo "=== $CC -std=c89 -pedantic ==="
	$CC -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC \
	    -c "$DIR/pgs_c89_check.c" -o "$DIR/pgs_c89_check.o"
	echo "    clean"
done
rm -f "$DIR/pgs_c89_check.o"

for CC in gcc clang; do
	command -v "$CC" >/dev/null 2>&1 || continue
	echo "=== $CC pgs_field_scanout ==="
	$CC -std=c89 -pedantic -Wall -Wextra -O2 $SANFLAGS -o "$DIR/pgs_field_scanout" "$DIR/pgs_field_scanout.c"
	"$DIR/pgs_field_scanout"
done

# -msse2 selects the scalar pair bodies, which is the shape an MSVC build
# below SSE4.1 takes; -msse4.1 selects the vector ones. Both have to be
# built and run, or the fallback is only a claim.
for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	for ISA in "-msse2" "-msse4.1"; do
	echo "=== $CXX $ISA ==="
	for t in pgs_layout_check pgs_vertex_oracle pgs_queue_equiv pgs_prim_record_equiv pgs_parallelogram_equiv pgs_kick_bench; do
		$CXX -O2 -std=c++17 $ISA $INC -o "$DIR/$t" "$DIR/$t.cpp"
	done
	"$DIR/pgs_layout_check"
	"$DIR/pgs_vertex_oracle"
	"$DIR/pgs_queue_equiv"
	"$DIR/pgs_prim_record_equiv"
	"$DIR/pgs_parallelogram_equiv"
	"$DIR/pgs_kick_bench" 2500 25
	done
done

# aarch64. The pair kernels have a NEON arm that no lane above can reach --
# x86 takes the SSE4.1 or the scalar body -- so until this ran, the arm the
# core's main target uses was only a claim. The bench is left out: a qemu
# figure would time the emulator, not the target.
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 &&
   command -v qemu-aarch64 >/dev/null 2>&1; then
	echo "=== aarch64 (NEON pair kernels) ==="
	command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 &&
		aarch64-linux-gnu-gcc -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 $INC \
		    -c "$DIR/pgs_c89_check.c" -o "$DIR/pgs_c89_check.o" &&
		rm -f "$DIR/pgs_c89_check.o"
	for t in pgs_layout_check pgs_vertex_oracle pgs_queue_equiv pgs_prim_record_equiv pgs_parallelogram_equiv; do
		aarch64-linux-gnu-g++ -O2 -std=c++17 -static $INC -o "$DIR/a_$t" "$DIR/$t.cpp"
		qemu-aarch64 "$DIR/a_$t"
		rm -f "$DIR/a_$t"
	done
else
	echo
	echo "skipping aarch64 lane (no cross toolchain or qemu)"
fi
