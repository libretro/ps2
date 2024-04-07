LIST(REMOVE_ITEM pcsx2GSSources
	GS/GSCapture.cpp
	GS/GSPng.cpp
)

set(pcsx2DEV9Sources)
set(pcsx2DEV9Headers)
set(pcsx2USBSources)
set(pcsx2USBHeaders)
set(pcsx2RecordingSources)
set(pcsx2RecordingVirtualPadResources)
set(pcsx2RecordingHeaders)
set(pcsx2ZipToolsSources)
set(pcsx2ZipToolsHeaders)

set(pcsx2FrontendSources)

target_link_libraries(PCSX2 PRIVATE
	${wxWidgets_LIBRARIES}
	${AIO_LIBRARIES}
	${GLIB_LIBRARIES}
	${GLIB_GIO_LIBRARIES}
)

#add_link_options(-fuse-ld=gold)
#add_link_options(-Wl,--gc-sections,--print-symbol-counts,sym.log)

target_sources(PCSX2 PRIVATE
   ${CMAKE_SOURCE_DIR}/libretro/main.cpp
   ${CMAKE_SOURCE_DIR}/libretro/options.cpp
   ${CMAKE_SOURCE_DIR}/libretro/input.cpp
   ${CMAKE_SOURCE_DIR}/common/GL/ContextRetroGL.cpp
#   USB/USBNull.cpp
   ${pcsx2LTOSources}
)

target_link_libraries(PCSX2 PRIVATE
	PCSX2_FLAGS
)
include_directories(. ${CMAKE_SOURCE_DIR}/libretro ${CMAKE_SOURCE_DIR}/common)
set_target_properties(PCSX2 PROPERTIES
   LIBRARY_OUTPUT_NAME pcsx2_libretro
   PREFIX ""
)

#   set(LIBRARY_OUTPUT_PATH "${CMAKE_BINARY_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
   set(CLANG 1)
endif()

if(NOT MSVC AND NOT CLANG)
   set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(PACKAGE_MODE)
    install(TARGETS PCSX2 DESTINATION ${BIN_DIR})
else(PACKAGE_MODE)
    install(TARGETS PCSX2 DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif(PACKAGE_MODE)
