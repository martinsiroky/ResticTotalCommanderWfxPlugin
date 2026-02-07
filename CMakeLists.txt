cmake_minimum_required(VERSION 3.15)
project(restic-wfx LANGUAGES C)

set(RESTIC_WFX_VERSION "1.0")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(restic_wfx SHARED
    src/plugin_main.c
    src/wfx_interface.c
    src/wfx_interface.h
    src/restic_process.c
    src/restic_process.h
    src/json_parse.c
    src/json_parse.h
    src/repo_config.c
    src/repo_config.h
    src/ls_cache.c
    src/ls_cache.h
    vendor/cJSON.c
    vendor/cJSON.h
    vendor/sqlite3.c
    vendor/sqlite3.h
    include/fsplugin.h
    restic_wfx.def
)

target_include_directories(restic_wfx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

# Output as .wfx64 for 64-bit Total Commander (.wfx is for 32-bit plugins)
set_target_properties(restic_wfx PROPERTIES
    SUFFIX ".wfx64"
    PREFIX ""
)

target_compile_definitions(restic_wfx PRIVATE
    WIN32
    _WINDOWS
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
)

target_link_libraries(restic_wfx PRIVATE
    shlwapi
    shell32
)

# Statically link MinGW C runtime to avoid external DLL dependencies
if (MINGW)
    target_link_options(restic_wfx PRIVATE
        -static-libgcc
    )
endif()

# Copy README.txt to build directory so the plugin can display it
add_custom_command(TARGET restic_wfx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/README.txt"
        "$<TARGET_FILE_DIR:restic_wfx>/README.txt"
    COMMENT "Copying README.txt to build directory"
)

# Create release package
set(RELEASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release")
set(STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/staging/restic_wfx")

add_custom_command(TARGET restic_wfx POST_BUILD
    # Create directories
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RELEASE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGING_DIR}"

    # Copy files to staging
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:restic_wfx>" "${STAGING_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/README.txt" "${STAGING_DIR}/"

    # Create zip (using PowerShell on Windows)
    COMMAND powershell -Command "Compress-Archive -Force -Path '${CMAKE_CURRENT_BINARY_DIR}/staging/restic_wfx' -DestinationPath '${RELEASE_DIR}/restic_wfx_${RESTIC_WFX_VERSION}.zip'"

    COMMENT "Creating release package: restic_wfx_${RESTIC_WFX_VERSION}.zip"
)
