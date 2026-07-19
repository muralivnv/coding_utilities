if(DEFINED PCRE2_SOURCE_DIR)
    add_subdirectory(${PCRE2_SOURCE_DIR} pcre2_build SYSTEM)
else()
    message(FATAL_ERROR "PCRE2 source not provided")
endif()

if(DEFINED MIO_SOURCE_DIR)
    add_subdirectory(${MIO_SOURCE_DIR} mio_build SYSTEM)
else()
    message(FATAL_ERROR "Mio source not provided")
endif()

find_package(SQLite3 REQUIRED)

add_subdirectory(tree-sitter)
add_library(external_libs INTERFACE)
target_link_libraries(external_libs INTERFACE
                      tree-sitter-core
                      tree-sitter-parsers
                      mio::mio
                      pcre2-8-static
                      SQLite::SQLite3)
