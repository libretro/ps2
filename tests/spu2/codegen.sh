#!/bin/sh
# SPU2 codegen census.
#
# Emits one line per emitted function: unit, symbol, instruction count and
# memory-op count. Memory ops are the number that tracks wall-clock in this
# tree; instruction count alone has repeatedly pointed the wrong way.
#
# Take a census before a change and after it, then diff the two files. A
# conversion that is meant to be neutral leaves both columns alone.
#
#   ./codegen.sh > /tmp/before
#   ...edit...
#   ./codegen.sh > /tmp/after
#   diff /tmp/before /tmp/after
#
# Run it under both compilers: the census covers g++ and clang++ and tags
# each line, so a change that only helps one of them is visible.
# Measured with the flags the core actually ships with. Makefile builds the
# emulator at -O3 -DNDEBUG -fno-strict-aliasing, and __fi is always_inline
# only under NDEBUG -- without it nothing inlines, the units come out a
# fraction of their real size, and the census describes a build nobody runs.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/SPU2 -I$ROOT/common"
INC="$INC -I$ROOT/libretro/libretro-common/include -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
UNITS="Mixer spu2sys ADSR Reverb ReverbResample RegTable ReadInput Dma spu2freeze spu2"

TMP=${TMPDIR:-/tmp}/spu2cg.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

for CXX in g++ clang++; do
	command -v "$CXX" >/dev/null 2>&1 || continue
	for u in $UNITS; do
		src=$ROOT/pcsx2/SPU2/$u.cpp
		[ -f "$src" ] || src=$ROOT/pcsx2/SPU2/$u.c
		[ -f "$src" ] || continue
		case $src in
		*.c) CC=$(echo "$CXX" | sed 's/++$//;s/^g$/gcc/')
		     $CC -O3 -DNDEBUG -fno-strict-aliasing -std=gnu89 -msse4.1 $INC -S "$src" -o "$TMP/u.s" ;;
		*)   $CXX -O3 -DNDEBUG -fno-strict-aliasing -std=c++17 -msse4.1 $INC -S "$src" -o "$TMP/u.s" ;;
		esac
		awk -v tag="$CXX" -v unit="$u" '
			/^[_A-Za-z][_A-Za-z0-9.$]*:$/ { cur=substr($0,1,length($0)-1); n=0; m=0; next }
			cur == "" { next }
			/^[ \t]+[a-z]/ {
				if ($1 ~ /^(ret|endbr64|nop|\.)/) next
				n++
				if ($0 ~ /[a-z0-9]\(%|\(%r|\(%e|[0-9a-zA-Z_]+\(%rip\)|\[[a-z0-9]/) m++
			}
			/\.size/ { if (cur != "" && n > 0) printf "%s\t%s\t%s\t%d\t%d\n", tag, unit, cur, n, m; cur="" }
		' "$TMP/u.s"
	done
done >"$TMP/raw"

# Names have to survive the port: a method, a free function and a static
# helper for the same work must land on the same line so before and after
# line up. Demangle, drop the class qualifier, the parameter list and the
# V_Core_ prefix the free functions carry.
c++filt <"$TMP/raw" | awk -F'\t' '
	{
		s = $3
		sub(/\(.*/, "", s)
		sub(/^V_Core::/, "", s)
		sub(/^V_Core_/, "", s)
		sub(/.*::/, "", s)
		sub(/\..*/, "", s)
		printf "%-8s %-14s %-44s %4d %4d\n", $1, $2, s, $4, $5
	}' | sort
