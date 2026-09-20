#!/bin/sh
# The Vulkan device-memory suballocator that replaces VMA
# (GS/Renderers/Vulkan/GSVulkanHeap.c), built and run without a GPU: the
# driver entry points are stubs, so what runs is the arithmetic.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)

${CC:-cc} -std=gnu89 -Wall -Wextra -O2 -g $SANFLAGS \
	-I "$ROOT/pcsx2/GS/Renderers/Vulkan" -I "$ROOT/3rdparty/vulkan-headers/include" \
	-o "$DIR/vkheap_test" \
	"$DIR/main.c" "$ROOT/pcsx2/GS/Renderers/Vulkan/GSVulkanHeap.c"
"$DIR/vkheap_test"
