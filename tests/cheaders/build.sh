#!/bin/sh
# Headers that both languages read.
#
# A header reachable from a .c file has to compile as C89 *and* as C++, and
# the two are easy to drift apart: a bare `struct Foo` used as a type name, a
# reference parameter, an alignas, a std:: declaration that crept in. None of
# those fail the C++ build, so nothing notices until a C unit includes the
# header -- by which time the fix is a cascade rather than a line.
#
# Every header listed here is compiled on its own, from C and from C++, with
# nothing included ahead of it: a header that only works when something else
# was included first is not self-sufficient, and that is how R5900.h came to
# depend on its includer for u32.
#
# Add a header here when a C unit starts including it. Removing one is a
# regression.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/common -I$ROOT/libretro/libretro-common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include"
INC="$INC -msse4.1"

HEADERS="
FreezeTypes.h
SaveState.h
MemoryTypes.h
R5900.h
R3000A.h
Counters.h
IopCounters.h
COP0.h
Elfheader.h
ps2/BiosTools.h
VirtualMemory.h
vtlb.h
Dmac.h
Hw.h
Memory.h
Common.h
Sif.h
VU.h
VUops.h
"

TMP=${TMPDIR:-/tmp}/cheaders.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail=0
for h in $HEADERS; do
	printf '%-22s' "$h"
	printf '#include "%s"\nint main(void) { return 0; }\n' "$h" > "$TMP/t.c"
	cp "$TMP/t.c" "$TMP/t.cpp"

	for CC in gcc clang; do
		command -v "$CC" >/dev/null 2>&1 || continue
		case $CC in gcc) CXX=g++ ;; clang) CXX=clang++ ;; esac

		if ! out=$($CC -std=gnu89 -Wno-comment -Werror=declaration-after-statement \
		                $INC -fsyntax-only "$TMP/t.c" 2>&1); then
			printf ' FAIL(%s C)\n%s\n' "$CC" "$out"
			fail=1
			continue 2
		fi
		if ! out=$($CXX -std=c++17 $INC -fsyntax-only "$TMP/t.cpp" 2>&1); then
			printf ' FAIL(%s C++)\n%s\n' "$CXX" "$out"
			fail=1
			continue 2
		fi
	done
	printf ' ok\n'
done

# The savestate units themselves, since they are what pulled these headers
# into C in the first place.
for u in SaveStateBase SaveStateFreeze Sif COP2; do
	printf '%-22s' "$u.c"
	gcc -std=gnu89 -Wall -Wextra -Wno-comment \
	    -Werror=declaration-after-statement \
	    $INC -fsyntax-only "$ROOT/pcsx2/$u.c" || fail=1
	printf ' ok\n'
done

[ "$fail" = 0 ] && echo "PASS: headers read as C89 and as C++" || echo "FAIL"
exit $fail
