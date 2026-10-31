#
# RMG-Ranked-SmashRemix CMakeLists.txt
#
project(RMG-Ranked-SmashRemix)

set(CMAKE_CXX_STANDARD 20)

# disable lib prefix
set(CMAKE_SHARED_LIBRARY_PREFIX "")

add_library(RMG-Ranked-SmashRemix SHARED SmashRemix.cpp)

# the host looks for <module>.dll and its data in <module>/
set_target_properties(RMG-Ranked-SmashRemix PROPERTIES
    OUTPUT_NAME SmashRemix
    CXX_VISIBILITY_PRESET hidden
)

target_include_directories(RMG-Ranked-SmashRemix PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../RMG-Core
)
