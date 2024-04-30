
add_library(pcsx2_libretro SHARED)

target_sources(pcsx2_libretro PRIVATE
   ${CMAKE_SOURCE_DIR}/libretro/main.cpp
   ${CMAKE_SOURCE_DIR}/libretro/DEV9.cpp
   ${CMAKE_SOURCE_DIR}/libretro/USB.cpp
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
	"${CMAKE_SOURCE_DIR}/libretro"
)

target_include_directories(common PUBLIC
	"${CMAKE_SOURCE_DIR}/libretro"
)

target_include_directories(pcsx2_libretro PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/libretro"
	"${CMAKE_SOURCE_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/3rdparty/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

#include_directories(. ${CMAKE_SOURCE_DIR}/libretro ${CMAKE_SOURCE_DIR}/common)
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
