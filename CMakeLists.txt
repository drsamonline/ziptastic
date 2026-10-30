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
# Engine core
#
# Phase 0 links nothing but itself. Later phases add vendored sources here:
#   Phase 1  vendor/hcs/old/xz       (liblzma, LZMA2 fallback)
#   Phase 8  vendor/hcs/old/preflate (DEFLATE recompressor)
#   Phase 9  vendor/hcs/old/libbsc, brotli-master, zstd-dev, lz4-dev
#   Phase 10 vendor/hcs/old/zpaq     (libzpaq, --max-ratio only)
# Nothing GPL/LGPL is ever added — see AGENT_PROMPT.md §2.
# ---------------------------------------------------------------------------

add_library(chce_core STATIC
  src/engine/block_writer.cpp
  src/engine/compressor.cpp
  src/engine/decompressor.cpp
)

target_include_directories(chce_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

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
