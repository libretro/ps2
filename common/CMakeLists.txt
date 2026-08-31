# Check that people use the good file
if(NOT TOP_CMAKE_WAS_SOURCED)
	message(FATAL_ERROR "
	You did not 'cmake' the good CMakeLists.txt file. Use the one in the top dir.
	It is advice to delete all wrongly generated cmake stuff => CMakeFiles & CMakeCache.txt")
endif(NOT TOP_CMAKE_WAS_SOURCED)

add_library(common)

target_sources(common PRIVATE
	AlignedMalloc.cpp
	Console.cpp
	FastJmp.cpp
	MemorySettingsInterface.cpp
	Path.cpp
	Semaphore.cpp
	SettingsWrapper.cpp
	StringUtil.cpp
	TextureDecompress.cpp
	Threads.cpp
	HostSys.cpp
)

# x86emitter sources: x86-only JIT code emitter. Excluded on aarch64, where the
# recompilers that use it are gated out and the interpreters run instead (arm64
# port, Phase B). There is no arm64 emitter yet (that is Phase C).
# The reference emitter implementation lives with the verification harness in
# tests/emitter/reference; the byte oracles compare the C89 core against it.
# With C89_EMITTER enabled the build emits through the header-only core in
# emitter/c89emit.h and compiles none of these.
if(_M_X86 AND NOT C89_EMITTER)
	target_sources(common PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/avx.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/groups.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/jmp.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy_sse.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/movs.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/simd.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/x86emitter.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/internal.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/legacy_internal.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/avx.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group1.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group2.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/group3.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/incdec.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/jmpcall.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/movs.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_arithmetic.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_comparisons.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_helpers.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_moremovs.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/simd_shufflepack.h
		${CMAKE_CURRENT_SOURCE_DIR}/../tests/emitter/reference/implement/test.h
	)
endif()

# x86emitter headers
target_sources(common PRIVATE
	Align.h
	AlignedMalloc.h
	Console.h
	CpuFeatures.h
	FastJmp.h
	General.h
	HashCombine.h
	MemorySettingsInterface.h
	Path.h
	RedtapeWilCom.h
	RedtapeWindows.h
	SettingsInterface.h
	SettingsWrapper.h
	StringUtil.h
	TextureDecompress.h
	Threading.h
	VectorIntrin.h
	WindowInfo.h
	emitter/c89compat.h
	emitter/c89emit.h
	emitter/c89ops.h
	emitter/x86emitter.h
	emitter/x86types.h
)

if(WIN32)
	enable_language(ASM_MASM)
	target_sources(common PRIVATE FastJmp.asm)
	target_link_libraries(common PUBLIC winmm)
	target_sources(common PRIVATE
		FastJmp.asm
	)
endif()

if(APPLE)
	target_compile_options(common PRIVATE -fobjc-arc)
	target_link_options(common PRIVATE -fobjc-link-runtime)
endif()

if (USE_GCC AND CMAKE_INTERPROCEDURAL_OPTIMIZATION)
	# GCC LTO doesn't work with asm statements
	set_source_files_properties(FastJmp.cpp PROPERTIES COMPILE_FLAGS -fno-lto)
endif()

if(UNIX AND NOT APPLE AND TARGET libbacktrace::libbacktrace)
	target_compile_definitions(common PRIVATE "HAS_LIBBACKTRACE=1")
	target_link_libraries(common PRIVATE libbacktrace::libbacktrace)
endif()

target_link_libraries(common PRIVATE
	${LIBC_LIBRARIES}
)

fixup_file_properties(common)
target_compile_features(common PUBLIC cxx_std_17)
target_include_directories(common PUBLIC ../3rdparty/include ../ ../libretro/libretro-common/include)
target_compile_definitions(common PUBLIC "${PCSX2_DEFS}")
target_compile_options(common PRIVATE "${PCSX2_WARNINGS}")
