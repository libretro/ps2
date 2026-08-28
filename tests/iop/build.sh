#!/bin/sh
# IOP (R3000A) core against ps2autotests console captures.
#
# Point PS2AUTOTESTS at a checkout:
#   git clone https://github.com/unknownbrackets/ps2autotests
#
# Usage: sh tests/iop/build.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
CC=${CC:-cc}
SANFLAGS=""
[ -n "$SANITIZER" ] && SANFLAGS="-fsanitize=$SANITIZER"

echo "== IOP multiply and divide vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/muldiv.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwmuldiv" "$DIR/hwmuldiv.c"
	"$DIR/iop_hwmuldiv" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP ALU vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/alu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwalu" "$DIR/hwalu.c"
	"$DIR/iop_hwalu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP loads and stores vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/lsu.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwlsu" "$DIR/hwlsu.c"
	"$DIR/iop_hwlsu" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== IOP branch conditions vs console =="
EXPECTED="${PS2AUTOTESTS:-}/tests/cpu/iop/branch.expected"
if [ -f "$EXPECTED" ]; then
	"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwbranch" "$DIR/hwbranch.c"
	"$DIR/iop_hwbranch" "$EXPECTED"
else
	echo "  skipped: set PS2AUTOTESTS to a ps2autotests checkout to run this"
fi

echo "== GTE divider self-checks (no capture needed) =="
"$CC" -O1 -g -Wall $SANFLAGS -o "$DIR/iop_hwgte" "$DIR/hwgte.c"
"$DIR/iop_hwgte"

echo "== GTE vs PS1 console captures =="
# https://github.com/JaCzekanski/ps1-tests -- point PS1TESTS at a checkout.
GTELOG="${PS1TESTS:-}/gte-fuzz/gte_valid_0xc0ffee_50.log"
if [ -f "$GTELOG" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/iop_hwgtefuzz" "$DIR/hwgtefuzz.cpp" "$ROOT/pcsx2/IopGte.cpp"
	"$DIR/iop_hwgtefuzz" "$GTELOG" | tail -1
else
	echo "  skipped: set PS1TESTS to a ps1-tests checkout to run this"
fi

echo "== MDEC vs PS1 console trace (report, not a gate) =="
MDECLOG="${PS1TESTS:-}/mdec/step-by-step-log/psx.log"
if [ -f "$MDECLOG" ]; then
	${CXX:-c++} -std=c++17 -O1 -w $SANFLAGS -I "$ROOT" -I "$ROOT/pcsx2" \
		-I "$ROOT/common" -I "$ROOT/common/include" -I "$ROOT/3rdparty/include" \
		-I "$ROOT/libretro/libretro-common/include" \
		-o "$DIR/iop_hwmdec" "$DIR/hwmdec.cpp" "$ROOT/pcsx2/Mdec.cpp"
	"$DIR/iop_hwmdec" "$MDECLOG" | tail -3 || true
else
	echo "  skipped: set PS1TESTS to a ps1-tests checkout to run this"
fi
