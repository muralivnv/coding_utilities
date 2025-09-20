add_library(gai_lib STATIC
            src/regex.cpp
            src/operation.cpp)
target_link_libraries(gai_lib PRIVATE external_libs common)
target_compile_options(gai_lib PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
          
add_executable(gai src/gai.cpp)
target_link_libraries(gai PRIVATE gai_lib external_libs common)
target_compile_options(gai PRIVATE ${ADDITIONAL_COMPILER_FLAGS})

add_executable(gai_tests src/tests.cpp)
target_link_libraries(gai_tests PRIVATE gai_lib external_libs common)
target_compile_options(gai_tests PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
