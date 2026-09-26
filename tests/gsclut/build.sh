#!/bin/sh
# Built as C89, like the core's C, and run.
#
# clut_bank_clamp  : the GPU palette copy against the CPU palette reader.
# clut_from_target : palettes the GPU drew, read from the target holding
#                    them: the sampled copy at scale, which target serves,
#                    and the load's CBP.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/clut_bank_clamp" "$DIR/clut_bank_clamp.c" 
"$DIR/clut_bank_clamp"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/clut_from_target" "$DIR/clut_from_target.c" -lm
"$DIR/clut_from_target"
