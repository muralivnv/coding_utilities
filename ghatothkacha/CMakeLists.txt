add_executable(ghatothkacha src/ghatothkacha.cpp)
target_link_libraries(ghatothkacha PRIVATE external_libs common)
target_compile_options(ghatothkacha PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
