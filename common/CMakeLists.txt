set(COMMON_SOURCES src/args.cpp
    src/mmap_stream.cpp src/mmap_file.cpp src/subprocess.cpp)

add_library(common STATIC ${COMMON_SOURCES})
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(common PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
