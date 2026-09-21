#!/bin/sh
# What the C units and the C++ units expect of each other's symbols.
#
# Two link failures have reached a delivered patch this way, and a compile
# check cannot see either of them:
#
#   - a C unit calls a function whose only definition is C++-mangled,
#     because the header declaring it has no extern "C". Both sides compile;
#     every call site fails at link. (SaveState_FreezeMem, then
#     vu0ExecMicro/_vu0FinishMicro/intDoBranch.)
#
#   - a C unit defines something the header gives internal linkage in C++
#     and external linkage in C -- a bare `const u32`, a bare `inline`
#     function -- so the second C unit including that header defines it
#     again and the link fails on the duplicate. (EELOAD_START and its
#     three neighbours, count_leading_zero.)
#
# Both are about symbols, so this compares symbols. Every .c in the tree is
# built, then every .cpp that builds here, and then:
#
#   1. no global symbol may be defined by two C objects, and
#   2. every symbol a C object leaves undefined must be defined somewhere
#      unmangled -- if the only match is a mangled one, the declaration the
#      C side saw and the definition the C++ side emitted disagree.
#
# The C++ half is built at -O0: this is about which symbols exist and how
# they are spelled, not about codegen. Expect a couple of minutes.
#
# The GS backends need SDK headers that are not always present. They are
# skipped, which is sound here because no C unit reaches them -- if one ever
# does, its symbols show up as findings rather than silence.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
INC="-I$ROOT -I$ROOT/pcsx2 -I$ROOT/pcsx2/x86 -I$ROOT/common -I$ROOT/common/include"
INC="$INC -I$ROOT/3rdparty -I$ROOT/3rdparty/include -I$ROOT/libretro/libretro-common/include"
SKIP='parallel-gs|Granite|DX11|DX12|OpenGL|Vulkan|uwp|pcap_io'
JOBS=${JOBS:-8}

TMP=${TMPDIR:-/tmp}/clinkage.$$
mkdir -p "$TMP/c" "$TMP/cxx"
trap 'rm -rf "$TMP"' EXIT

obj() { echo "$1" | sed "s|^$ROOT/||" | tr '/' '_' | sed 's/\.[a-z]*$/.o/'; }

for f in $(find "$ROOT/pcsx2" -name '*.c' | grep -vE "$SKIP" | sort); do
	gcc -O2 -DNDEBUG -std=gnu89 -msse4.1 -fno-strict-aliasing $INC \
	    -c "$f" -o "$TMP/c/$(obj "$f")" 2>/dev/null ||
		{ echo "FAIL: $f does not compile as C"; exit 1; }
done
echo "C objects  : $(ls "$TMP/c" | wc -l)"

# xargs does the throttling: a `&` loop with a jobs-based limit does not
# hold in a non-interactive shell, and launching every unit at once buries
# the machine.
find "$ROOT/pcsx2" "$ROOT/libretro" -name '*.cpp' | grep -vE "$SKIP" | sort |
while read -r f; do printf '%s\0%s\0' "$f" "$TMP/cxx/$(obj "$f")"; done |
xargs -0 -n2 -P "$JOBS" sh -c 'g++ -O0 -DNDEBUG -std=c++17 -DPCSX2_CORE '"$INC"' -c "$1" -o "$2" 2>/dev/null || true' _
echo "C++ objects: $(ls "$TMP/cxx" | wc -l)"

nm "$TMP"/c/*.o   | awk '$2 ~ /^[TDBRG]$/   { print $3 }' | sort    > "$TMP/def_c_all"
sort -u "$TMP/def_c_all" > "$TMP/def_c"
nm "$TMP"/cxx/*.o | awk '$2 ~ /^[TDBRGWVi]$/ { print $3 }' | sort -u > "$TMP/def_cxx"
nm "$TMP"/c/*.o   | awk '$1 == "U"          { print $2 }' | sort -u > "$TMP/undef"

echo
echo "== no global symbol defined by two C units =="
dup=$(uniq -d "$TMP/def_c_all")
if [ -n "$dup" ]; then
	echo "$dup" | sed 's/^/  FAIL: defined more than once: /'
	exit 1
fi
echo "  ok"

echo
echo "== every symbol the C units need is spelled the way C spells it =="
bad=0
while read -r sym; do
	case $sym in ''|_GLOBAL_*|__*|*@*) continue ;; esac
	grep -qxF "$sym" "$TMP/def_c"   && continue
	grep -qxF "$sym" "$TMP/def_cxx" && continue
	# Nothing defines it unmangled. A mangled match means this tree does
	# define it, just not with the linkage the C side was promised;
	# anything else is libc or libstdc++ and not ours to resolve.
	# _Z<length><name><argument codes>: the length prefix pins the name
	# exactly, and the codes that follow are letters, so anchoring on a
	# non-identifier character after it would never match.
	if grep -qE "^_Z${#sym}${sym}([^A-Za-z0-9_]|[A-Za-z].*)?$" "$TMP/def_cxx"; then
		echo "  FAIL: $sym is defined only C++-mangled;"
		echo "        its declaration needs extern \"C\""
		bad=1
	fi
done < "$TMP/undef"
[ "$bad" = 0 ] && echo "  ok"

echo
[ "$bad" = 0 ] || exit 1
echo "PASS: the C and C++ halves agree on linkage"
