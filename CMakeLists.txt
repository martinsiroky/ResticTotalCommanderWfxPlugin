cmake_minimum_required(VERSION 3.15)
project(restic-wfx LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(restic_wfx SHARED
    src/plugin_main.c
    src/wfx_interface.c
    src/wfx_interface.h
    include/fsplugin.h
    restic_wfx.def
)

target_include_directories(restic_wfx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Output as .wfx64 for 64-bit Total Commander (.wfx is for 32-bit plugins)
set_target_properties(restic_wfx PROPERTIES
    SUFFIX ".wfx64"
    PREFIX ""
)

target_compile_definitions(restic_wfx PRIVATE
    WIN32
    _WINDOWS
)

target_link_libraries(restic_wfx PRIVATE
    shlwapi
)

# Statically link MinGW C runtime to avoid external DLL dependencies
if (MINGW)
    target_link_options(restic_wfx PRIVATE
        -static-libgcc
    )
endif()
