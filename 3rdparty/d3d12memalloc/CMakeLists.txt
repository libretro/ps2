add_library(D3D12MemAlloc
	include/D3D12MemAlloc.h
	src/D3D12MemAlloc.cpp
)

target_include_directories(D3D12MemAlloc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# libretro: the atomics are retro_atomic-backed (see the D3D12MA_ATOMIC block
# in the header), so <retro_atomic.h> has to be reachable. The static Makefile
# has -I$(LIBRETRO_COMM_DIR)/include globally; this target needed telling.
target_include_directories(D3D12MemAlloc PUBLIC
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include")

