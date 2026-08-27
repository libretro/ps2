#-------------------------------------------------------------------------------
#                       Search all libraries on the system
#-------------------------------------------------------------------------------
if(EXISTS ${PROJECT_SOURCE_DIR}/.git)
	find_package(Git)
endif()
# zlib and libpng are gone: every former consumer decodes through
# libretro-common (rinflate/rpng), compiled into the libretro target.
if (WIN32)
	# We bundle everything on Windows
	add_subdirectory(3rdparty/D3D12MemAlloc EXCLUDE_FROM_ALL)
else()
	# Using find_package OpenGL without either setting your opengl preference to GLVND or LEGACY
	# is deprecated as of cmake 3.11.
	# Android has no desktop libGL/GLX to find; the GL renderer resolves
	# every entry point through hw_render.get_proc_address via glad, so
	# nothing needs linking there.
	if(USE_OPENGL AND NOT ANDROID)
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

# zstd, libzip, lzma, and chdr are gone with their vendored trees: CHD
# decodes through rchd, ZIP members and CSO/gz through rinflate, all in
# libretro-common (see libretro/CMakeLists.txt for the source list).

# arm64 recompiler port (Phase C): VIXL is the AArch64 code emitter. x86 builds
# use the in-tree x86 emitter (common/emitter) instead, so this is arm64-only.
if(NOT _M_X86)
	add_subdirectory(3rdparty/vixl EXCLUDE_FROM_ALL)
endif()

if(USE_OPENGL)
	add_subdirectory(3rdparty/glad EXCLUDE_FROM_ALL)
endif()

if(USE_VULKAN)
	add_subdirectory(3rdparty/glslang EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/vulkan-headers EXCLUDE_FROM_ALL)
endif()
