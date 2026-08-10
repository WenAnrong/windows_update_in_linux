cmake_minimum_required(VERSION 3.16)
project(windows_update_in_linux VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output the single runnable binary directly into the repo root, so that
# "./windows_update_in_linux" works right after building — no install needed.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

# fontconfig is optional: enables family-name font lookup, else we fall back
# to scanning common system font directories.
pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)

set(SOURCES
    src/main.c
    src/ttydrm.c
)

add_executable(windows_update_in_linux ${SOURCES})
target_link_libraries(windows_update_in_linux PRIVATE
    PkgConfig::DRM
    PkgConfig::FREETYPE
    m   # sin/cos for the spinner (libm)
)

if(FONTCONFIG_FOUND)
    target_compile_definitions(windows_update_in_linux PRIVATE HAVE_FONTCONFIG=1)
    target_link_libraries(windows_update_in_linux PRIVATE PkgConfig::FONTCONFIG)
endif()
