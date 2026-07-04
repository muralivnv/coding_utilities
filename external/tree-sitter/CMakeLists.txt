# --- Tree-sitter Core ---
if(DEFINED TREE_SITTER_SOURCE_DIR)
    add_library(tree-sitter STATIC)
    target_sources(tree-sitter
        PRIVATE
            "${TREE_SITTER_SOURCE_DIR}/lib/src/lib.c"
    )
    target_include_directories(tree-sitter
        PRIVATE
            $<BUILD_INTERFACE:${TREE_SITTER_SOURCE_DIR}/lib/src>
        PUBLIC
            $<INSTALL_INTERFACE:include>
            $<BUILD_INTERFACE:${TREE_SITTER_SOURCE_DIR}/lib/include>
    )
else()
    message(FATAL_ERROR "TREE_SITTER_SOURCE_DIR not defined. Build with Nix.")
endif()

# --- Helper Function Replacement ---
# Instead of CPM, we expect the path to be passed via cmakeFlags
function(add_grammar_from_nix NAME SOURCE_DIR)
    add_library(${NAME} STATIC)

    # Check for scanner.c (optional)
    file(GLOB maybe_scanner "${SOURCE_DIR}/src/scanner.c")

    target_sources(${NAME}
        PRIVATE
            "${SOURCE_DIR}/src/parser.c"
            ${maybe_scanner}
    )
    target_include_directories(${NAME}
        PRIVATE
            $<BUILD_INTERFACE:${SOURCE_DIR}/src>
        PUBLIC
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(${NAME}
        INTERFACE
            tree-sitter)
endfunction()

# --- Load Grammars ---
add_grammar_from_nix(tree-sitter-c      ${TREE_SITTER_C_SOURCE_DIR})
add_grammar_from_nix(tree-sitter-cpp    ${TREE_SITTER_CPP_SOURCE_DIR})
add_grammar_from_nix(tree-sitter-python ${TREE_SITTER_PYTHON_SOURCE_DIR})

# --- Interfaces (Same as before) ---
add_library(tree-sitter-core INTERFACE)
target_include_directories(tree-sitter-core
    INTERFACE
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(tree-sitter-core
    INTERFACE
        tree-sitter
)

add_library(tree-sitter-parsers INTERFACE)
target_link_libraries(tree-sitter-parsers
                      INTERFACE
                        tree-sitter-c
                        tree-sitter-cpp
                        tree-sitter-python)   