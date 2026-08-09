set(GAI_LIB_SOURCES src/regex.cpp
    src/operation.cpp
    src/input.cpp
    src/process.cpp)

add_library(gai_lib STATIC ${GAI_LIB_SOURCES})
target_link_libraries(gai_lib PRIVATE external_libs common)
target_compile_options(gai_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(gai src/gai.cpp)
target_link_libraries(gai PRIVATE gai_lib external_libs common)
target_compile_options(gai PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

# Compiled through an object library so the test sources do not sit behind the
# gai binary. The tests do need it -- see below -- but only once they run, and a
# target-level dependency on an executable holds back compiles as well as the
# link, which left this compile stranded between gai's link and its own.
add_library(gai_test_lib OBJECT src/tests.cpp)
target_link_libraries(gai_test_lib PRIVATE gai_lib external_libs common)
target_compile_options(gai_test_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(gai_tests)
target_link_libraries(gai_tests PRIVATE gai_test_lib gai_lib external_libs common)
set_target_properties(gai_tests PROPERTIES LINKER_LANGUAGE CXX)
# The exit status and stderr of the CLI are only observable by running it, so
# the tests spawn the binary sitting next to them; it has to be built first.
add_dependencies(gai_tests gai)
add_test(NAME gai_tests COMMAND gai_tests)

# Nothing here imports a C++ module. Left on, the scan is a second preprocess of
# every source and a barrier besides: ninja compiles nothing in a target until
# every file in it has been scanned.
set_target_properties(gai_lib gai gai_test_lib gai_tests
                      PROPERTIES CXX_SCAN_FOR_MODULES OFF)
