# Check that people use the good file
if(NOT TOP_CMAKE_WAS_SOURCED)
	message(FATAL_ERROR "
	You did not 'cmake' the good CMakeLists.txt file. Use the one in the top dir.
	It is advice to delete all wrongly generated cmake stuff => CMakeFiles & CMakeCache.txt")
endif(NOT TOP_CMAKE_WAS_SOURCED)

add_library(common)

# x86emitter sources
target_sources(common PRIVATE
	AlignedMalloc.cpp
	Console.cpp
	FastJmp.cpp
	FileSystem.cpp
	MemorySettingsInterface.cpp
	MD5Digest.cpp
	Semaphore.cpp
	SettingsWrapper.cpp
	StringUtil.cpp
	TextureDecompress.cpp
	Timer.cpp
	emitter/avx.cpp
	emitter/groups.cpp
	emitter/jmp.cpp
	emitter/legacy.cpp
	emitter/legacy_sse.cpp
	emitter/movs.cpp
	emitter/simd.cpp
	emitter/x86emitter.cpp
	Threads.cpp
	HostSys.cpp
)

# x86emitter headers
target_sources(common PRIVATE
	Align.h
	AlignedMalloc.h
	boost_spsc_queue.hpp
	Console.h
	FastJmp.h
	FileSystem.h
	General.h
	HashCombine.h
	MemorySettingsInterface.h
	MD5Digest.h
	Path.h
	RedtapeWilCom.h
	RedtapeWindows.h
	SettingsInterface.h
	SettingsWrapper.h
	StringUtil.h
	Timer.h
	TextureDecompress.h
	Threading.h
	VectorIntrin.h
	WindowInfo.h
	emitter/implement/group1.h
	emitter/implement/group2.h
	emitter/implement/group3.h
	emitter/implement/incdec.h
	emitter/implement/jmpcall.h
	emitter/implement/movs.h
	emitter/implement/simd_arithmetic.h
	emitter/implement/simd_comparisons.h
	emitter/implement/simd_helpers.h
	emitter/implement/simd_moremovs.h
	emitter/implement/simd_shufflepack.h
	emitter/implement/test.h
	emitter/instructions.h
	emitter/internal.h
	emitter/legacy_instructions.h
	emitter/legacy_internal.h
	emitter/x86emitter.h
	emitter/x86types.h
)

if(WIN32)
	enable_language(ASM_MASM)
	target_sources(common PRIVATE FastJmp.asm)
	target_link_libraries(common PUBLIC WIL::WIL winmm)
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
	PNG::PNG
)

target_link_libraries(common PUBLIC
	fmt::fmt
	fast_float
)

fixup_file_properties(common)
target_compile_features(common PUBLIC cxx_std_17)
target_include_directories(common PUBLIC ../3rdparty/include ../)
target_compile_definitions(common PUBLIC "${PCSX2_DEFS}")
target_compile_options(common PRIVATE "${PCSX2_WARNINGS}")
