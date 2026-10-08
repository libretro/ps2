#!/bin/sh
# ohci_dma_copy. See main.c.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
SAN=""
if [ -n "$SANITIZER" ]; then
	SAN="-fsanitize=$SANITIZER"
elif echo 'int main(void){return 0;}' | ${CC:-cc} -x c -fsanitize=address -o /dev/null - 2>/dev/null; then
	SAN="-fsanitize=address"
fi
${CC:-cc} -std=c89 -pedantic -Wall -Wextra -O1 $SAN -I "$ROOT/pcsx2" \
	-o "$DIR/ohcidma" "$DIR/main.c" "$ROOT/pcsx2/USB/libretro-usb/ohci_dma.c"
"$DIR/ohcidma"
