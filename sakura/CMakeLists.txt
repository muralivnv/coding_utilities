add_executable(sakura src/sakura.cpp src/config.cpp)
target_link_libraries(sakura PRIVATE external_libs common)
target_compile_options(sakura PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
