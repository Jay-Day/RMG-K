#
# RMG-Core CMakeLists.txt
#
project(RMG-Core)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED minizip)
if (WIN32)
    pkg_check_modules(ICONV REQUIRED iconv)
endif(WIN32)

if (NOT PORTABLE_INSTALL)
    include(GNUInstallDirs)
endif(NOT PORTABLE_INSTALL)

configure_file(Config.hpp.in Config.hpp)

set(RMG_CORE_SOURCES
    m64p/Api.cpp
    m64p/CoreApi.cpp
    m64p/ConfigApi.cpp
    m64p/PluginApi.cpp
    m64p/Handle.cpp
    CachedRomHeaderAndSettings.cpp
    ConvertStringEncoding.cpp
    SpeedLimiter.cpp
    SpeedFactor.cpp
    RomSettings.cpp
    Directories.cpp
    MediaLoader.cpp
    Screenshot.cpp
    RomHeader.cpp
    Emulation.cpp
    SaveState.cpp
    RollbackNetcode.cpp
    rmgk_gekko.cpp
    Callback.cpp
    Settings.cpp
    Archive.cpp
    Library.cpp
    Netplay.cpp
    Kaillera.cpp
    Plugins.cpp
    Version.cpp
    Cheats.cpp
    String.cpp
    Volume.cpp
    VidExt.cpp
    Video.cpp
    Error.cpp
    Core.cpp
    File.cpp
    Key.cpp
    Rom.cpp
)

if (NETPLAY)
    add_definitions(-DNETPLAY)
endif(NETPLAY)

if (PORTABLE_INSTALL)
    add_definitions(-DPORTABLE_INSTALL)
endif(PORTABLE_INSTALL)

add_library(RMG-Core SHARED ${RMG_CORE_SOURCES})

if(TARGET GekkoNet)
    target_compile_definitions(RMG-Core PRIVATE RMGK_HAVE_GEKKONET)
    target_link_libraries(RMG-Core PRIVATE GekkoNet)
endif()

if(UNIX)
    target_link_libraries(RMG-Core PRIVATE dl)
endif(UNIX)

if(WIN32)
    target_link_libraries(RMG-Core PRIVATE wsock32 ws2_32 ${ICONV_LIBRARIES})
endif(WIN32)

target_link_libraries(RMG-Core PRIVATE
    ${MINIZIP_LIBRARIES}
    lzma
)

# Link n02 static library for Kaillera netplay support
# PRIVATE prevents n02 from being transitively linked into RMG.exe,
# which would create duplicate static variables (two copies of n02 state).
# --export-all-symbols forces the linker to export n02 functions from
# RMG-Core.dll even though CORE_EXPORT (__declspec(dllexport)) triggers
# selective export mode.
if (TARGET n02)
    target_compile_definitions(RMG-Core PRIVATE RMGK_HAVE_P2P_TRANSPORT)
    target_link_libraries(RMG-Core PRIVATE n02)
    target_include_directories(RMG-Core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../n02
    )
    if (WIN32)
        target_link_options(RMG-Core PRIVATE -Wl,--export-all-symbols)
    endif()
endif()

target_include_directories(RMG-Core PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../
    ${MINIZIP_INCLUDE_DIRS}
)

set_target_properties(RMG-Core PROPERTIES CXX_VISIBILITY_PRESET hidden)
