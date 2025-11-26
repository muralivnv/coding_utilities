add_executable(jack src/subprocess.cpp src/commands.cpp src/jack.cpp)
target_link_libraries(jack PRIVATE external_libs common)
target_compile_options(jack PRIVATE ${ADDITIONAL_COMPILER_FLAGS})
