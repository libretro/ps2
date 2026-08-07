
add_library(pcsx2_libretro SHARED)

target_sources(pcsx2_libretro PRIVATE
   ${CMAKE_SOURCE_DIR}/libretro/main.cpp
   ${CMAKE_SOURCE_DIR}/libretro/patches.cpp
   ${CMAKE_SOURCE_DIR}/libretro/DEV9.cpp
   ${CMAKE_SOURCE_DIR}/libretro/USB.cpp
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_strl.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/compat_posix_string.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/compat/fopen_utf8.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_utf.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/file_path.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/file_path_io.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/string/stdstring.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/file_stream.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/time/rtime.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/rthreads/rthreads.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/queues/retro_spsc.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/vfs/vfs_implementation.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/vfs/vfs_hybrid.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/file/retro_dirent.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/features/features_cpu.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/chd/rchd.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/flac/rflac.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/7z/r7z_lzma.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_rzstd.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_huffman.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_deflate.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_crc32.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/encodings/encoding_rlz4.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/formats/png/rpng.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream_deflate.c
   ${CMAKE_SOURCE_DIR}/libretro/libretro-common/streams/trans_stream_pipe.c
)

target_link_libraries(pcsx2_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(APPLE)
  target_link_libraries(pcsx2_libretro PRIVATE
    "-framework CoreGraphics"
    "-framework IOKit"
    "-framework Foundation"
  )
endif()

target_include_directories(PCSX2_FLAGS INTERFACE
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(common PUBLIC
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
)

target_include_directories(pcsx2_libretro PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/libretro/libretro-common/include"
	"${CMAKE_SOURCE_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/3rdparty/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

set_target_properties(pcsx2_libretro PROPERTIES
   LIBRARY_OUTPUT_NAME pcsx2_libretro
   PREFIX ""
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
   set(CLANG 1)
endif()

if(NOT MSVC AND NOT CLANG)
   set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${BIN_DIR})
else(PACKAGE_MODE)
    install(TARGETS pcsx2_libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif(PACKAGE_MODE)
