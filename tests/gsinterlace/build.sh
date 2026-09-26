#!/bin/sh
# Built as C89, like the core's C, and run.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for t in line_weave field_mad fastmad_rule sw_fields; do
	${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
		-o "$DIR/$t" "$DIR/$t.c" -lm
done
"$DIR/line_weave"
"$DIR/field_mad"
"$DIR/sw_fields"
"$DIR/fastmad_rule" "$DIR/../../pcsx2/GS/parallel-gs/gs/shaders/slangmosh.hpp"
