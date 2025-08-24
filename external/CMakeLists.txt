## Get CPM
##########
set(CPM_VERSION 0.40.2)
set(CPM_PATH "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_VERSION}.cmake")

if (NOT(EXISTS ${CPM_PATH}))
  message(STATUS "Downloading CPM.cmake")
  file(DOWNLOAD
       "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
       ${CPM_PATH})
endif()

set(FETCHCONTENT_QUIET FALSE)

include(${CPM_PATH})

CPMAddPackage(
  NAME mio
  VERSION 1.0.0
  URL https://github.com/vimpunk/mio/archive/8b6b7d878c89e81614d05edca7936de41ccdd2da.tar.gz
  OPTIONS
    "mio.tests OFF"
    "mio.installation OFF"
)

CPMAddPackage(
  NAME argy
  VERSION 0.5.0
  URL https://github.com/mshenoda/argy/archive/refs/tags/v0.5.0.tar.gz
  OPTIONS
    "BUILD_EXAMPLES OFF"
)

add_subdirectory(tree-sitter)
add_library(external_libs INTERFACE)
target_link_libraries(external_libs INTERFACE
                      # data-structures
                      tree-sitter-core
                      tree-sitter-parsers
                      mio::mio
                      argy)
