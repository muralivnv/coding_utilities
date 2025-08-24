cmake_minimum_required(VERSION 3.20)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

project(treesitter_tags LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH "$ORIGIN")
set(CMAKE_INSTALL_MESSAGE LAZY)

set(COMMON_FLAGS -fPIC -Wall -Wpedantic -Wextra -Wfloat-equal
    -Wno-sign-conversion -Wno-unused-function -Wno-unused-parameter
    -fdiagnostics-color=always -Wno-unused-variable -Wno-unused-local-typedefs
    -Wno-missing-field-initializers)

set(DEBUG_COMPILER_FLAGS ${COMMON_FLAGS} -g3 -O0 -fno-omit-frame-pointer)
set(RELEASE_COMPILER_FLAGS ${COMMON_FLAGS} -O3 -msse4.2 -mavx2 -march=native -flto)
set(ADDITIONAL_COMPILER_FLAGS -Werror -Wshadow -Wimplicit-fallthrough
    -D_GLIBCXX_ASSERTIONS)

add_compile_options(
  "$<$<CONFIG:Debug>:${DEBUG_COMPILER_FLAGS}>"
  "$<$<CONFIG:Release>:${RELEASE_COMPILER_FLAGS}>"
)

add_subdirectory(external)
add_executable(${PROJECT_NAME}
               src/treesitter_tags.cpp
               src/config.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE external_libs)
target_compile_options(${PROJECT_NAME} PRIVATE ${ADDITIONAL_COMPILER_FLAGS})


