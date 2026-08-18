cmake_minimum_required(VERSION 4.1.0)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_BINARY_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}")

project(coding_utilities VERSION 26.09.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release binaries are link-time-optimised and stripped at the link. LTO
# measured smaller AND slightly faster here (size_report.md: koi -10%, symbol
# extraction -4%, clean builds faster); -s removes .symtab/.strtab, which is
# never mapped at runtime -- Release carries no DWARF, and crash handling does
# not symbolise, so nothing shipped reads what -s removes. Debug keeps both.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
add_link_options("$<$<CONFIG:Release>:-s>")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH "$ORIGIN")
set(CMAKE_INSTALL_MESSAGE LAZY)

set(COMMON_FLAGS -fPIC -Wall -Wpedantic -Wextra -Wfloat-equal
    -Wno-sign-conversion -Wno-unused-function -Wno-unused-parameter
    -fdiagnostics-color=always -Wno-unused-variable -Wno-unused-local-typedefs
    -Wno-missing-field-initializers)

set(DEBUG_COMPILER_FLAGS ${COMMON_FLAGS} -g3 -O0 -fno-omit-frame-pointer)
set(RELEASE_COMPILER_FLAGS ${COMMON_FLAGS} -O3 -msse4.2 -mavx2)
set(ADDITIONAL_COMPILER_FLAGS -Werror -Wshadow -Wimplicit-fallthrough -DPCRE2_CODE_UNIT_WIDTH=8 -DPROJECT_VERSION="${PROJECT_VERSION}")

add_compile_options(
  "$<$<CONFIG:Debug>:${DEBUG_COMPILER_FLAGS}>"
  "$<$<CONFIG:Release>:${RELEASE_COMPILER_FLAGS}>"
)

enable_testing()

# project
#########
add_subdirectory(external)

add_subdirectory(common)
add_subdirectory(gai)
add_subdirectory(ghatothkacha)
add_subdirectory(tooey)
add_subdirectory(koi)
