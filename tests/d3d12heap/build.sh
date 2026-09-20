#!/bin/sh
# The D3D12 heap suballocator that replaces D3D12MemoryAllocator
# (GS/Renderers/DX12/GSD3D12Heap.c), built and run without a GPU: the
# device entry points are stubs, so what runs is the arithmetic.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

${CC:-cc} -std=gnu89 -Wall -Wextra -O2 -g $SANFLAGS \
	-I "$ROOT/pcsx2/GS/Renderers/DX12" \
	-o "$DIR/d3d12heap_test" \
	"$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/DX12/GSD3D12Heap.c"
"$DIR/d3d12heap_test"
