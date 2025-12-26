add_custom_target(jack ALL
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jack
        ${CMAKE_CURRENT_BINARY_DIR}/jack
)
