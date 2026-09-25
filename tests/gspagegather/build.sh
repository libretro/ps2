#!/bin/sh
# Built as C89, like the core's C, and run.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O2 -g $SANFLAGS \
	-o "$DIR/page_gather" "$DIR/page_gather.c"
"$DIR/page_gather"
