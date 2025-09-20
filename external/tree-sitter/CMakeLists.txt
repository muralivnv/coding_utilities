CPMAddPackage(
  NAME tree-sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  VERSION 0.25.9
  DOWNLOAD_ONLY YES
)

if (tree-sitter_ADDED)
  add_library(tree-sitter STATIC)
  target_sources(tree-sitter
    PRIVATE
      "${tree-sitter_SOURCE_DIR}/lib/src/lib.c"
  )
  target_include_directories(tree-sitter
    PRIVATE
      $<BUILD_INTERFACE:${tree-sitter_SOURCE_DIR}/lib/src>
    PUBLIC
      $<INSTALL_INTERFACE:include>
      $<BUILD_INTERFACE:${tree-sitter_SOURCE_DIR}/lib/include>
  )

endif()


function(add_grammar_from_repo NAME REPO VERSION)
  CPMAddPackage(
    NAME ${NAME}
    GIT_REPOSITORY ${REPO}
    VERSION ${VERSION}
    DOWNLOAD_ONLY YES
  )

  if ("${${NAME}_ADDED}")
    add_library(${NAME} STATIC)

    file(GLOB maybe_scanner "${${NAME}_SOURCE_DIR}/src/scanner.c")
    target_sources(${NAME}
      PRIVATE
        "${${NAME}_SOURCE_DIR}/src/parser.c"
        ${maybe_scanner}
    )
    target_include_directories(${NAME}
      PRIVATE
        # parser.h is stored within the src directory, so we need to include
        # src in the search paths
        $<BUILD_INTERFACE:${${NAME}_SOURCE_DIR}/src>
      PUBLIC
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(${NAME}
      INTERFACE
        tree-sitter)

  endif()
endfunction(add_grammar_from_repo)


add_library(tree-sitter-core INTERFACE)
target_include_directories(tree-sitter-core
  INTERFACE
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tree-sitter-core
  INTERFACE
    tree-sitter
)

################################ Language Parsers ###################################
#####################################################################################
add_grammar_from_repo(tree-sitter-c      https://github.com/tree-sitter/tree-sitter-c         0.24.1)
add_grammar_from_repo(tree-sitter-cpp    https://github.com/tree-sitter/tree-sitter-cpp       0.23.4)
add_grammar_from_repo(tree-sitter-python https://github.com/tree-sitter/tree-sitter-python    0.25.0)

################################# LANGUAGE INTERFACE ################################
#####################################################################################
add_library(tree-sitter-parsers INTERFACE)
target_link_libraries(tree-sitter-parsers
                      INTERFACE
                        tree-sitter-c
                        tree-sitter-cpp
                        tree-sitter-python)
