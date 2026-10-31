cmake_minimum_required(VERSION 3.20)

project(ziptastic
  VERSION 0.1.0
  DESCRIPTION "CHCE — Chimera Hybrid Compression Engine"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CHCE_BUILD_TESTS "Build the test suite" ON)
option(CHCE_WERROR "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Vendored liblzma (Phase 1 — the Tier-1 LZMA2 fallback)
#
# Built from vendor/hcs/old/xz-master with its own CMake support, out of tree
# and untouched. We only need the library: tools, translations and xz's own
# test suite are all switched off so they do not leak into our build or ctest.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "" FORCE)

set(XZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/xz-master)
add_subdirectory(${XZ_DIR} ${CMAKE_BINARY_DIR}/xz EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Engine core
#
# Phase 0 links nothing but itself; Phase 1 adds liblzma. Later phases add
# more vendored sources here:
#   Phase 8  vendor/hcs/old/preflate (DEFLATE recompressor)
#   Phase 9  vendor/hcs/old/libbsc, brotli-master, zstd-dev, lz4-dev
#   Phase 10 vendor/hcs/old/zpaq     (libzpaq, --max-ratio only)
# Nothing GPL/LGPL is ever added — see AGENT_PROMPT.md §2.
# ---------------------------------------------------------------------------

add_library(chce_core STATIC
  src/engine/block_writer.cpp
  src/engine/compressor.cpp
  src/engine/decompressor.cpp
  src/engine/match_finder.cpp
  src/codecs/lzma2_fallback.cpp
)

target_include_directories(chce_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(chce_core PRIVATE liblzma)
target_include_directories(chce_core PRIVATE ${XZ_DIR}/src/liblzma/api)

if(MSVC)
  target_compile_options(chce_core PRIVATE /W4)
else()
  target_compile_options(chce_core PRIVATE -Wall -Wextra)
endif()

if(CHCE_WERROR)
  if(MSVC)
    target_compile_options(chce_core PRIVATE /WX)
  else()
    target_compile_options(chce_core PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

add_executable(ziptastic src/main.cpp)
target_link_libraries(ziptastic PRIVATE chce_core)
target_include_directories(ziptastic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(CHCE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS ziptastic RUNTIME DESTINATION bin)
