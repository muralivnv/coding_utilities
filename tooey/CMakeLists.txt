add_library(tooey_lib STATIC
            src/text.cpp
            src/layout.cpp)
target_link_libraries(tooey_lib PRIVATE external_libs common unistring_lib)
target_compile_options(tooey_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(tooey src/tooey.cpp)
target_link_libraries(tooey PRIVATE tooey_lib external_libs common unistring_lib)
target_compile_options(tooey PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(tooey_tests src/tests.cpp)
target_link_libraries(tooey_tests PRIVATE tooey_lib external_libs common unistring_lib)
target_compile_options(tooey_tests PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
add_test(NAME tooey_tests COMMAND tooey_tests)
