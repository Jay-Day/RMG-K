#
# RMG-Scripting CMakeLists.txt
#
project(RMG-Scripting)

set(CMAKE_CXX_STANDARD 20)

# Lua is found by parent CMakeLists.txt when SCRIPTING is enabled
if (NOT LUA_FOUND)
    message(FATAL_ERROR "RMG-Scripting added without Lua; check top-level SCRIPTING option")
endif()

set(RMG_SCRIPTING_SOURCES
    ScriptEngine.cpp
    ScriptManager.cpp
    MemoryModule.cpp
    EventModule.cpp
)

set(RMG_SCRIPTING_HEADERS
    ScriptEngine.hpp
    ScriptManager.hpp
    MemoryModule.hpp
    EventModule.hpp
)

add_library(RMG-Scripting STATIC ${RMG_SCRIPTING_SOURCES})

target_link_libraries(RMG-Scripting
    RMG-Core
    ${LUA_LIBRARIES}
)

target_include_directories(RMG-Scripting PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${LUA_INCLUDE_DIR}
)

target_include_directories(RMG-Scripting PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)
