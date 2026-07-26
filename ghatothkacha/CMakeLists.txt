add_library(ghatothkacha_lib STATIC
            src/protocol.cpp)
target_link_libraries(ghatothkacha_lib PRIVATE external_libs common)
target_include_directories(ghatothkacha_lib PUBLIC src)
target_compile_options(ghatothkacha_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(ghatothkacha src/ghatothkacha.cpp)
target_link_libraries(ghatothkacha PRIVATE ghatothkacha_lib external_libs common)
target_compile_options(ghatothkacha PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(ghatothkacha_tests src/tests.cpp)
target_link_libraries(ghatothkacha_tests PRIVATE ghatothkacha_lib external_libs common)
target_compile_options(ghatothkacha_tests PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
add_test(NAME ghatothkacha_tests COMMAND ghatothkacha_tests)
