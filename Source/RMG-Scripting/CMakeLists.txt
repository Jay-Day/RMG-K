#
# RMG-Scripting CMakeLists.txt
#
project(RMG-Scripting)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

set(RMG_SCRIPTING_SOURCES
    ScriptEngine.cpp
    ScriptManager.cpp
    MemoryModule.cpp
    EventModule.cpp
    InputModule.cpp
    NetworkModule.cpp
    NetplayModule.cpp
)

add_library(RMG-Scripting STATIC ${RMG_SCRIPTING_SOURCES})

target_link_libraries(RMG-Scripting
    RMG-Core
    quickjs
    ${CURL_LIBRARIES}
)

target_include_directories(RMG-Scripting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${CMAKE_SOURCE_DIR}/Source/3rdParty/quickjs
    ${CURL_INCLUDE_DIRS}
)

target_include_directories(RMG-Scripting PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)
