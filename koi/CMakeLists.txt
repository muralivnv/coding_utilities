set(KOI_LIB_SOURCES
    src/piece_tree.cpp
    src/piece_doc.cpp
    src/unicode.cpp
    src/selection.cpp
    src/keymap.cpp
    src/commands.cpp
    src/crash.cpp
    src/shell.cpp
    src/editor.cpp
    src/indent.cpp
    src/search.cpp
    src/query.cpp
    src/syntax.cpp
    src/symbols.cpp
    src/cli.cpp
    src/overview.cpp
    src/textobject.cpp
    src/theme.cpp
    src/render.cpp
    src/jumplist.cpp
    src/keylog.cpp
    src/project.cpp
    src/navigate.cpp
    src/sidebar.cpp
    src/error.cpp)

add_library(koi_lib STATIC ${KOI_LIB_SOURCES})
target_include_directories(koi_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(koi_lib PRIVATE ${CMAKE_SOURCE_DIR}/gai/src)
target_link_libraries(koi_lib PRIVATE common gai_lib external_libs ${CMAKE_DL_LIBS})
target_compile_definitions(koi_lib PRIVATE
                           KOI_GRAMMAR_DIR="${TREE_SITTER_GRAMMAR_DIR}"
                           KOI_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(koi_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(koi src/koi.cpp)
add_dependencies(koi tree-sitter-grammars)
target_link_libraries(koi PRIVATE koi_lib common external_libs)
target_compile_options(koi PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

# One file per subject, named after the source file it covers where the tests
# belong to one; the rest -- excerpt, leap, windows, undo_history, files -- are
# named after the feature they cut across. tests.h names every suite, main.cpp
# runs them.
set(KOI_TEST_SOURCES
    src/tests/main.cpp
    src/tests/test_support.cpp
    src/tests/cli.cpp
    src/tests/commands.cpp
    src/tests/crash.cpp
    src/tests/editor.cpp
    src/tests/excerpt.cpp
    src/tests/files.cpp
    src/tests/indent.cpp
    src/tests/jumplist.cpp
    src/tests/keylog.cpp
    src/tests/keymap.cpp
    src/tests/leap.cpp
    src/tests/navigate.cpp
    src/tests/overview.cpp
    src/tests/piece_doc.cpp
    src/tests/piece_tree.cpp
    src/tests/project.cpp
    src/tests/query.cpp
    src/tests/render.cpp
    src/tests/search.cpp
    src/tests/selection.cpp
    src/tests/shell.cpp
    src/tests/sidebar.cpp
    src/tests/sqlite.cpp
    src/tests/symbols.cpp
    src/tests/syntax.cpp
    src/tests/textobject.cpp
    src/tests/theme.cpp
    src/tests/thread_pool.cpp
    src/tests/undo_history.cpp
    src/tests/unicode.cpp
    src/tests/windows.cpp)

# The test objects are built through an object library so that compiling them
# does not wait on the koi executable. koi_tests does need that binary -- some
# suites run it -- but only when the test runs, and a target-level dependency
# on it makes ninja hold back every one of these compiles until koi has linked,
# which in turn waits on all of koi_lib. Against an object library the
# dependency lands on the link step alone, so the test files compile alongside
# koi_lib instead of after it.
add_library(koi_test_lib OBJECT ${KOI_TEST_SOURCES})
# gai/src so the search tests can reach the regex layer koi wraps: FindInText
# always asks for a JIT, and the interpreter -- the engine on no-W^X kernels and
# in the TSan build -- is only reachable by compiling a pattern with jit off.
target_include_directories(koi_test_lib PRIVATE ${CMAKE_SOURCE_DIR}/gai/src)
target_link_libraries(koi_test_lib PRIVATE koi_lib common gai_lib external_libs)
target_compile_options(koi_test_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS} -O1)
# Every test file includes tests.h, which reaches every koi header plus sqlite3
# and the regex layer: an empty test file costs ~6s to compile, and there are 31
# of them. Precompiled once, that parse is paid once instead of per file.
target_precompile_headers(koi_test_lib PRIVATE src/tests/tests.h)

add_executable(koi_tests)
target_link_libraries(koi_tests PRIVATE koi_test_lib koi_lib common gai_lib external_libs)
set_target_properties(koi_tests PROPERTIES LINKER_LANGUAGE CXX)
add_dependencies(koi_tests koi)
add_test(NAME koi_tests COMMAND koi_tests)

# Nothing here imports a C++ module. Left on, the scan is a second preprocess of
# every source and a barrier besides: ninja compiles nothing in a target until
# every file in it has been scanned.
set_target_properties(koi_lib koi koi_test_lib koi_tests
                      PROPERTIES CXX_SCAN_FOR_MODULES OFF)
