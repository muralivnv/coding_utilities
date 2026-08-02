add_library(common STATIC src/args.cpp
            src/mmap_stream.cpp src/mmap_file.cpp src/subprocess.cpp)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(common PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
