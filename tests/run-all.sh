#!/bin/sh
# Run every suite under tests/ and summarise.
#
# The suites fall into two groups. Six score the emulator against console
# captures and need a checkout to compare against:
#
#   PS2AUTOTESTS=/path/to/ps2autotests   (ee, fpu, iop, mmi, vif, vu)
#   PS1TESTS=/path/to/ps1-tests          (iop: the GTE and MDEC oracles)
#
#     git clone https://github.com/unknownbrackets/ps2autotests
#     git clone https://github.com/JaCzekanski/ps1-tests
#
# Without those the capture-backed suites skip rather than fail, so this
# is still worth running with neither set -- the rest do not need them.
#
# SANITIZER is passed through, so
#
#   SANITIZER=undefined sh tests/run-all.sh
#
# builds everything with UBSan. Do a clean build between sanitizers with
# different define sets rather than reusing objects.
#
# Exits non-zero if any suite does, and says which.
set -u

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Every directory with a build.sh, found rather than listed. A suite added
# to a hardcoded list by hand is a suite that silently stops running when
# someone forgets, and four had already stopped: gs, settings, cdvdread
# and mtvuring were all in the tree and none of them were in the list.
SUITES=$(for d in "$DIR"/*/build.sh; do
	[ -f "$d" ] || continue
	b=${d%/build.sh}
	printf '%s ' "${b##*/}"
done)

failed=""
ran=""

for s in $SUITES; do
	printf '\n======== %s ========\n' "$s"
	if sh "$DIR/$s/build.sh"; then
		ran="$ran $s"
	else
		failed="$failed $s"
	fi
done

printf '\n======== summary ========\n'
[ -n "$ran" ]     && printf 'passed: %s\n' "${ran# }"

if [ -n "$failed" ]; then
	printf 'FAILED: %s\n' "${failed# }"
	exit 1
fi

if [ -z "${PS2AUTOTESTS:-}" ]; then
	printf '\nPS2AUTOTESTS was not set, so the console-scored suites only\n'
	printf 'reported the checks that need no capture. Set it for the rest.\n'
fi
exit 0
