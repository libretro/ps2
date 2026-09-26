#!/bin/sh
# Built as C89, like the core's C, and run.
#
# readback_retire : a readback region is retired only once the readback ran.
# upload_dirty    : an EE upload into a target at another buffer width marks
#                   exactly the target pixels it wrote.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/readback_retire" "$DIR/readback_retire.c" 
"$DIR/readback_retire"
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/upload_dirty" "$DIR/upload_dirty.c"
"$DIR/upload_dirty"
