if(DEFINED PCRE2_SOURCE_DIR)
    add_subdirectory(${PCRE2_SOURCE_DIR} pcre2_build EXCLUDE_FROM_ALL SYSTEM)
    # Silence warnings
    target_compile_options(pcre2-8-static PRIVATE -w)
else()
    message(FATAL_ERROR "PCRE2 source not provided")
endif()

if (NOT DEFINED TERMBOX_SOURCE_DIR)
    message(FATAL_ERROR "Termbox source not provided")
endif()

find_library(UNISTRING_LIBRARY NAMES unistring REQUIRED)
find_path(UNISTRING_INCLUDE_DIR NAMES unistr.h REQUIRED)
add_library(unistring_lib INTERFACE)
target_include_directories(unistring_lib SYSTEM INTERFACE ${UNISTRING_INCLUDE_DIR})
target_link_libraries(unistring_lib INTERFACE ${UNISTRING_LIBRARY})

find_package(SQLite3 REQUIRED)

add_subdirectory(tree-sitter)
add_library(external_libs INTERFACE)
target_include_directories(external_libs INTERFACE
                           ${TERMBOX_SOURCE_DIR})
target_link_libraries(external_libs INTERFACE
                      tree-sitter-core
                      tree-sitter-parsers
                      pcre2-8-static
                      SQLite::SQLite3)
