# Vendored subset of VIXL (https://github.com/Linaro/vixl) -- AArch64 backend
# only, used as the code emitter for the lrps2 arm64 recompiler port.
# Upstream commit recorded in VIXL_VERSION.txt. License: see LICENCE (BSD-3).
#
# Only the aarch64 target is vendored (no aarch32). The code buffer uses mmap so
# VIXL manages the executable JIT memory itself.

file(GLOB VIXL_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/aarch64/*.cc"
)

add_library(vixl STATIC ${VIXL_SOURCES})

target_include_directories(vixl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(vixl PUBLIC cxx_std_17)

target_compile_definitions(vixl PUBLIC
	VIXL_INCLUDE_TARGET_AARCH64
)

# The staging buffer's growth path: mremap() on Linux, plain realloc on
# platforms without it. Darwin has no mremap, and upstream VIXL builds
# with the malloc backend there as well.
if(APPLE)
	target_compile_definitions(vixl PUBLIC VIXL_CODE_BUFFER_MALLOC)
else()
	target_compile_definitions(vixl PUBLIC VIXL_CODE_BUFFER_MMAP)
endif()

# VIXL is third-party; don't let the core's stricter warnings fail its build.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(vixl PRIVATE -w)
endif()
