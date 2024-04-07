add_library(glad
	src/glad.c
)

target_include_directories(glad PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(glad INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(glad PRIVATE Threads::Threads "${CMAKE_DL_LIBS}")

if(APPLE)
	target_compile_options(glad PRIVATE -fno-common)
endif()

