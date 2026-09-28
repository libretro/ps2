#!/bin/sh
# A change to the paraLLEl-GS rasterizer shaders must leave the code of
# every grid it does not touch as it was.
#
# ubershader.comp and triangle_setup.comp are specialised on the grid
# (constants 0 and 1, the log2 sample counts across and down), so a branch
# added for one grid should fold away for the others. This builds both
# shaders from the tree and from a git ref (origin/master unless given),
# freezes the grid constants to each grid the renderer offers, optimises,
# and compares the two builds in canonical form (pgs_spv_canon.c: ids
# renumbered by first use, declarations sorted). Every grid must come out
# identical, except those named in EXPECT_DIFFERENT (as "xy" pairs, comma
# separated), which must not: for a change that adds a grid, naming it
# is the control that the comparison can see a change at all.
#
#   sh tests/pgs/grid_fold.sh [ref]
#   EXPECT_DIFFERENT=23 sh tests/pgs/grid_fold.sh origin/master
#
# Needs glslc, spirv-opt and spirv-dis (apt install glslang-tools
# spirv-tools, or the shaderc package for glslc).
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
REF=${1:-origin/master}
SHADERS=pcsx2/GS/parallel-gs/gs/shaders
NEW=$ROOT/$SHADERS
OUT=${TMPDIR:-/tmp}/pgs-grid-fold
OLD=$OUT/ref
GRIDS="0 0|0 1|1 1|0 2|1 2|2 2|1 3|2 3"
EXPECT_DIFFERENT=${EXPECT_DIFFERENT:-""}

for tool in glslc spirv-opt spirv-dis; do
	command -v $tool >/dev/null 2>&1 || { echo "$tool not installed"; exit 1; }
done

rm -rf "$OUT"; mkdir -p "$OLD"
for f in $(git -C "$ROOT" ls-tree --name-only "$REF" -- "$SHADERS/"); do
	git -C "$ROOT" show "$REF:$f" > "$OLD/$(basename "$f")"
done

${CC:-cc} -O2 -std=c89 -pedantic -Wall -o "$OUT/canon" "$DIR/pgs_spv_canon.c"

build() {
	# build <dir> <stage> <source> <out> [defines...]
	d=$1; stage=$2; src=$3; out=$4; shift 4
	glslc -O --target-env=vulkan1.1 -fshader-stage=$stage -I"$d" "$@" -o "$out" "$d/$src"
}

fold() {
	# fold <in.spv> <x> <y> <out.txt> [more constants]
	spirv-opt --set-spec-const-default-value="0:$2 1:$3${5:+ $5}" --freeze-spec-const \
		--fold-spec-const-op-composite -O --eliminate-dead-const -O "$1" -o "$4.spv"
	spirv-dis --no-header "$4.spv" | "$OUT/canon" > "$4"
}

fail=0
for v in "ubershader.comp:00:-DFEEDBACK_COLOR=0 -DFEEDBACK_DEPTH=0" \
         "ubershader.comp:10:-DFEEDBACK_COLOR=1 -DFEEDBACK_DEPTH=0" \
         "ubershader.comp:01:-DFEEDBACK_COLOR=0 -DFEEDBACK_DEPTH=1" \
         "ubershader.comp:11:-DFEEDBACK_COLOR=1 -DFEEDBACK_DEPTH=1" \
         "triangle_setup.comp:ts:"; do
	src=${v%%:*}; rest=${v#*:}; tag=${rest%%:*}; defs=${rest#*:}
	# triangle_setup reads the grid's sampling offset only with the
	# super-sampled textures constant (2) on; freeze it on so that code
	# is live for the comparison.
	more=""; [ "$tag" = ts ] && more="2:true"
	build "$OLD" comp "$src" "$OUT/old_$tag.spv" $defs
	build "$NEW" comp "$src" "$OUT/new_$tag.spv" $defs
	echo "$GRIDS" | tr '|' '\n' | while read -r x y; do
		fold "$OUT/old_$tag.spv" "$x" "$y" "$OUT/old_${tag}_$x$y.txt" "$more"
		fold "$OUT/new_$tag.spv" "$x" "$y" "$OUT/new_${tag}_$x$y.txt" "$more"
		if cmp -s "$OUT/old_${tag}_$x$y.txt" "$OUT/new_${tag}_$x$y.txt"; then r=same; else r=different; fi
		want=same
		for g in $(echo "$EXPECT_DIFFERENT" | tr ',' ' '); do
			[ "$g" = "$x$y" ] && want=different
		done
		if [ "$r" = "$want" ]; then ok=ok; else ok=FAIL; echo FAIL >> "$OUT/failed"; fi
		printf '  %-20s grid (%s,%s): %-9s %s\n' "$src${defs:+ $defs}" "$x" "$y" "$r" "$ok"
	done
done

if [ -f "$OUT/failed" ]; then
	echo "FAIL: a grid's folded code changed where it should not, or stayed where it should change"
	exit 1
fi
echo "PASS: the grids the change leaves alone fold to the code they did"
