project(quickjs C)

set(QUICKJS_SOURCES
    quickjs.c
    cutils.c
    libregexp.c
    libunicode.c
    dtoa.c
    quickjs-libc.c
)

add_library(quickjs STATIC ${QUICKJS_SOURCES})

target_include_directories(quickjs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION="2025-04-26"
    CONFIG_BIGNUM
    _GNU_SOURCE
)

# Suppress warnings from vendored code
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(quickjs PRIVATE
        -w
        -fno-strict-aliasing
    )
endif()

set_property(TARGET quickjs PROPERTY POSITION_INDEPENDENT_CODE ON)
