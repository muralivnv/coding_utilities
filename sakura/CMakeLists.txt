add_library(sakura_lib STATIC
            src/config.cpp
            src/extract.cpp)
target_link_libraries(sakura_lib PRIVATE external_libs common)
target_include_directories(sakura_lib PUBLIC src)
target_compile_options(sakura_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(sakura src/sakura.cpp)
target_link_libraries(sakura PRIVATE sakura_lib external_libs common)
target_compile_options(sakura PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(sakura_tests src/tests.cpp)
target_link_libraries(sakura_tests PRIVATE sakura_lib external_libs common)
target_compile_options(sakura_tests PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
add_test(NAME sakura_tests COMMAND sakura_tests)
