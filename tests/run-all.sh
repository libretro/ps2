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
SUITES="mmi vu fpu ee iop vif emitter faultstress fpaudit ipu spsc"

failed=""
skipped=""
ran=""

for s in $SUITES; do
	[ -f "$DIR/$s/build.sh" ] || { skipped="$skipped $s"; continue; }
	printf '\n======== %s ========\n' "$s"
	if sh "$DIR/$s/build.sh"; then
		ran="$ran $s"
	else
		failed="$failed $s"
	fi
done

printf '\n======== summary ========\n'
[ -n "$ran" ]     && printf 'passed: %s\n' "${ran# }"
[ -n "$skipped" ] && printf 'missing build.sh: %s\n' "${skipped# }"

if [ -n "$failed" ]; then
	printf 'FAILED: %s\n' "${failed# }"
	exit 1
fi

if [ -z "${PS2AUTOTESTS:-}" ]; then
	printf '\nPS2AUTOTESTS was not set, so the console-scored suites only\n'
	printf 'reported the checks that need no capture. Set it for the rest.\n'
fi
exit 0
