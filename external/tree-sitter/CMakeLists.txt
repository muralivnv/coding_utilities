if(NOT DEFINED TREE_SITTER_SOURCE_DIR)
    message(FATAL_ERROR "TREE_SITTER_SOURCE_DIR not defined. Build with Nix.")
endif()

# --- Tree-sitter runtime ---
add_library(tree-sitter SHARED "${TREE_SITTER_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(tree-sitter
    PRIVATE
        $<BUILD_INTERFACE:${TREE_SITTER_SOURCE_DIR}/lib/src>
    SYSTEM PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${TREE_SITTER_SOURCE_DIR}/lib/include>
)
set_target_properties(tree-sitter PROPERTIES SOVERSION 0)
target_compile_options(tree-sitter PRIVATE -w)

# --- Grammars ---
if(NOT DEFINED TREE_SITTER_GRAMMARS)
    message(FATAL_ERROR "TREE_SITTER_GRAMMARS not defined. Build with Nix.")
endif()

function(add_grammar NAME SOURCE_DIR)
    file(GLOB maybe_scanner "${SOURCE_DIR}/src/scanner.c" "${SOURCE_DIR}/src/scanner.cc")

    add_library(tree-sitter-${NAME} SHARED "${SOURCE_DIR}/src/parser.c" ${maybe_scanner})
    target_include_directories(tree-sitter-${NAME}
        PRIVATE
            $<BUILD_INTERFACE:${SOURCE_DIR}/src>
    )
    target_include_directories(tree-sitter-${NAME}
        SYSTEM PRIVATE ${TREE_SITTER_SOURCE_DIR}/lib/include)
    target_compile_options(tree-sitter-${NAME} PRIVATE -w)
    set_target_properties(tree-sitter-${NAME} PROPERTIES PREFIX "lib")
endfunction()

set(_grammar_names "")
foreach(entry IN LISTS TREE_SITTER_GRAMMARS)
    if(NOT entry MATCHES "^([A-Za-z0-9_]+)=(.+)$")
        message(FATAL_ERROR "TREE_SITTER_GRAMMARS entry is not name=path: ${entry}")
    endif()
    add_grammar(${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    list(APPEND _grammar_names ${CMAKE_MATCH_1})
endforeach()

set(TREE_SITTER_GRAMMAR_DIR "${CMAKE_CURRENT_BINARY_DIR}" CACHE INTERNAL "")
set(TREE_SITTER_GRAMMAR_NAMES "${_grammar_names}" CACHE INTERNAL "")

add_custom_target(tree-sitter-grammars)
foreach(name IN LISTS _grammar_names)
    add_dependencies(tree-sitter-grammars tree-sitter-${name})
endforeach()

# --- Interfaces ---
add_library(tree-sitter-core INTERFACE)
target_link_libraries(tree-sitter-core INTERFACE tree-sitter)
add_dependencies(tree-sitter-core tree-sitter-grammars)