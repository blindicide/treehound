# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

# The single authoritative version.  Everything else (binaries, packages,
# --version, release tag checks) derives from here.
project(treehound
    VERSION 0.1.0
    DESCRIPTION "Indexed file search and disk-usage explorer for Linux"
    HOMEPAGE_URL "https://github.com/blindicide/treehound"
    LANGUAGES C)

include(GNUInstallDirs)

option(TREEHOUND_BUILD_GUI "Build the GTK4 graphical interface" ON)
option(TREEHOUND_BUILD_TESTS "Build unit and integration tests" ON)
option(TREEHOUND_WERROR "Treat compiler warnings as errors" OFF)
option(TREEHOUND_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_definitions(_GNU_SOURCE _FILE_OFFSET_BITS=64)
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(TREEHOUND_WERROR)
    add_compile_options(-Werror)
endif()
if(TREEHOUND_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 3.34 REQUIRED)

configure_file(include/treehound/version.h.in
    ${PROJECT_BINARY_DIR}/include/treehound/version.h @ONLY)

# ---- th_base: shared by every program; no SQLite, no GTK -------------------
add_library(th_base STATIC
    src/core/strbuf.c
    src/core/json.c
    src/core/utf8.c
    src/core/match.c
    src/core/util.c
    src/core/config.c
    src/ipc/ipc.c
)
target_include_directories(th_base PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/include)

# ---- th_engine: database, indexer and search; only treehoundd links it -----
add_library(th_engine STATIC
    src/database/db.c
    src/indexer/indexer.c
    src/indexer/mounts.c
    src/search/search.c
)
target_link_libraries(th_engine PUBLIC th_base SQLite::SQLite3 Threads::Threads)

if(TREEHOUND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
