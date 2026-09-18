# Check that people use the good file
if(NOT TOP_CMAKE_WAS_SOURCED)
	message(FATAL_ERROR "
	You did not 'cmake' the good CMakeLists.txt file. Use the one in the top dir.
	It is advice to delete all wrongly generated cmake stuff => CMakeFiles & CMakeCache.txt")
endif(NOT TOP_CMAKE_WAS_SOURCED)

# No sources of its own: what used to be common/ is libretro-common's now,
# and what is left here is headers plus the include paths, definitions and
# link libraries every consumer needs. An INTERFACE library is what CMake
# calls that. A static one would ask ar to build an archive with no members,
# which GNU ar tolerates and Apple's and MSVC's lib reject -- and it only
# ever had members on x86, so a Linux build never saw it.
add_library(common INTERFACE)


# The headers are not listed: an INTERFACE target builds nothing, and they
# are found through the include directories below like any other header.

if(WIN32)
	target_link_libraries(common INTERFACE winmm)
endif()

if(APPLE)
	target_compile_options(common INTERFACE -fobjc-arc)
	target_link_options(common INTERFACE -fobjc-link-runtime)
endif()

if(UNIX AND NOT APPLE AND TARGET libbacktrace::libbacktrace)
	target_compile_definitions(common INTERFACE "HAS_LIBBACKTRACE=1")
	target_link_libraries(common INTERFACE libbacktrace::libbacktrace)
endif()

target_link_libraries(common INTERFACE
	${LIBC_LIBRARIES}
)

fixup_file_properties(common)
target_compile_features(common INTERFACE cxx_std_17)
# ../pcsx2: common/ is being folded into it and includes its headers
# (SLockGuard.h, HostMem.h); the Makefile build has always had it on
# the include path, the CMake build had not.
target_include_directories(common INTERFACE ../3rdparty/include ../ ../pcsx2 ../libretro/libretro-common/include)
target_compile_definitions(common INTERFACE "${PCSX2_DEFS}")
target_compile_options(common INTERFACE "${PCSX2_WARNINGS}")
