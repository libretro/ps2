# Once done, this will define
#
# LIBC_FOUND - system has libc
# LIBC_LIBRARIES - link these to use libc

if(LIBC_LIBRARIES)
	set(LIBC_FIND_QUIETLY TRUE)
endif(LIBC_LIBRARIES)

find_library(libm NAMES m)

# OSX doesn't have rt. On Linux timer and aio dependency.
if(ANDROID)
	# bionic has neither librt nor libdl: the timer and dl entry points live in
	# libc itself. Looking for them leaves librt-NOTFOUND in LIBC_LIBRARIES,
	# which CMake then refuses to generate a link line for.
	set(LIBC_LIBRARIES ${libm})
elseif(APPLE)
	find_library(libdl NAMES dl)
	set(LIBC_LIBRARIES ${librt} ${libdl} ${libm})
elseif(Linux)
	find_library(libdl NAMES dl)
	find_library(librt NAMES rt)
	set(LIBC_LIBRARIES ${librt} ${libdl} ${libm})
elseif(ANDROID)
	# bionic folds the realtime (clock_*, timer_*, aio_*) and dynamic loader
	# entry points straight into libc: there is no librt and no libdl to find.
	# Searching for them anyway leaves the literal librt-NOTFOUND in
	# LIBC_LIBRARIES, which reaches the link interface of the `common` and
	# `PCSX2` targets and aborts the NDK lanes at generate time with
	# "variables are used in this project, but they are set to NOTFOUND".
	set(LIBC_LIBRARIES ${libm})
else()
	# FreeBSD doesn't have libdl
	find_library(librt NAMES rt)
	set(LIBC_LIBRARIES ${librt} ${libm})
endif()

# handle the QUIETLY and REQUIRED arguments and set LIBC_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libc DEFAULT_MSG LIBC_LIBRARIES)

mark_as_advanced(LIBC_LIBRARIES)

