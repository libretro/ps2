#-------------------------------------------------------------------------------
#                       Search all libraries on the system
#-------------------------------------------------------------------------------
if(EXISTS ${PROJECT_SOURCE_DIR}/.git)
	find_package(Git)
endif()
if(APPLE)
	find_package(ZLIB REQUIRED)
	add_subdirectory(3rdparty/libpng EXCLUDE_FROM_ALL)
else()
	add_subdirectory(3rdparty/libpng EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/zlib EXCLUDE_FROM_ALL)
endif()
if (WIN32)
	# We bundle everything on Windows
	add_subdirectory(3rdparty/wil EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/D3D12MemAlloc EXCLUDE_FROM_ALL)
else()
	find_package(LibLZMA REQUIRED)
	make_imported_target_if_missing(LibLZMA::LibLZMA LIBLZMA)

	# Using find_package OpenGL without either setting your opengl preference to GLVND or LEGACY
	# is deprecated as of cmake 3.11.
	if(USE_OPENGL)
		set(OpenGL_GL_PREFERENCE GLVND)
		find_package(OpenGL REQUIRED)
	endif()
	# On macOS, Mono.framework contains an ancient version of libpng.  We don't want that.
	# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
	if(APPLE)
	else()
		set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
		set(CMAKE_FIND_FRAMEWORK NEVER)
	endif()

	## Use pcsx2 package to find module
	include(FindLibc)
endif(WIN32)

# Require threads on all OSes.
find_package(Threads REQUIRED)

# Blacklist bad GCC
if(GCC_VERSION VERSION_EQUAL "7.0" OR GCC_VERSION VERSION_EQUAL "7.1")
	GCC7_BUG()
endif()

if((GCC_VERSION VERSION_EQUAL "9.0" OR GCC_VERSION VERSION_GREATER "9.0") AND GCC_VERSION LESS "9.2")
	message(WARNING "
	It looks like you are compiling with 9.0.x or 9.1.x. Using these versions is not recommended,
	as there is a bug known to cause the compiler to segfault while compiling. See patch
	https://gitweb.gentoo.org/proj/gcc-patches.git/commit/?id=275ab714637a64672c6630cfd744af2c70957d5a
	Even with that patch, compiling with LTO may still segfault. Use at your own risk!
	This text being in a compile log in an open issue may cause it to be closed.")
endif()

find_optional_system_library(fmt 3rdparty/fmt/fmt 7.1.3)
find_optional_system_library(ryml 3rdparty/rapidyaml/rapidyaml 0.4.0)
find_optional_system_library(zstd 3rdparty/zstd 1.4.5)
if (${zstd_TYPE} STREQUAL System)
	alias_library(Zstd::Zstd zstd::libzstd_shared)
	alias_library(pcsx2-zstd zstd::libzstd_shared)
endif()
find_optional_system_library(libzip 3rdparty/libzip 1.8.0)

add_subdirectory(3rdparty/lzma EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/chdr EXCLUDE_FROM_ALL)

# rapidyaml includes fast_float as a submodule, saves us pulling it in directly.
# Normally, we'd just pull in the cmake project, and link to it, but... it seems to enable
# permissive mode, which breaks other parts of PCSX2. So, we'll just create a target here
# for now.
#add_subdirectory(3rdparty/rapidyaml/rapidyaml/ext/c4core/src/c4/ext/fast_float EXCLUDE_FROM_ALL)
add_library(fast_float INTERFACE)
target_include_directories(fast_float INTERFACE 3rdparty/rapidyaml/rapidyaml/ext/c4core/src/c4/ext/fast_float/include)

add_subdirectory(3rdparty/cpuinfo EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/lz4 EXCLUDE_FROM_ALL)

if(USE_OPENGL)
	add_subdirectory(3rdparty/glad EXCLUDE_FROM_ALL)
endif()

if(USE_VULKAN)
	add_subdirectory(3rdparty/glslang EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/vulkan-headers EXCLUDE_FROM_ALL)
endif()
