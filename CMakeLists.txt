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


# ---------------------------------------------------------------------------
# Preflate (Phase 8) — DEFLATE recompressor, Apache-2.0.
#
# Compiled directly rather than through its own CMakeLists: that file declares
# cmake_minimum_required(3.2), which CMake 4 refuses, and it also builds two demo
# executables we do not want. The file list is the one its LIB_SRC declares.
# ---------------------------------------------------------------------------
set(PREFLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/preflate-master)
set(PREFLATE_SRC
  ${PREFLATE_DIR}/preflate_block_decoder.cpp
  ${PREFLATE_DIR}/preflate_block_reencoder.cpp
  ${PREFLATE_DIR}/preflate_block_trees.cpp
  ${PREFLATE_DIR}/preflate_complevel_estimator.cpp
  ${PREFLATE_DIR}/preflate_constants.cpp
  ${PREFLATE_DIR}/preflate_decoder.cpp
  ${PREFLATE_DIR}/preflate_hash_chain.cpp
  ${PREFLATE_DIR}/preflate_info.cpp
  ${PREFLATE_DIR}/preflate_parameter_estimator.cpp
  ${PREFLATE_DIR}/preflate_parser_config.cpp
  ${PREFLATE_DIR}/preflate_predictor_state.cpp
  ${PREFLATE_DIR}/preflate_reencoder.cpp
  ${PREFLATE_DIR}/preflate_seq_chain.cpp
  ${PREFLATE_DIR}/preflate_statistical_codec.cpp
  ${PREFLATE_DIR}/preflate_statistical_model.cpp
  ${PREFLATE_DIR}/preflate_token.cpp
  ${PREFLATE_DIR}/preflate_token_predictor.cpp
  ${PREFLATE_DIR}/preflate_tree_predictor.cpp
  ${PREFLATE_DIR}/support/arithmetic_coder.cpp
  ${PREFLATE_DIR}/support/array_helper.cpp
  ${PREFLATE_DIR}/support/bit_helper.cpp
  ${PREFLATE_DIR}/support/bitstream.cpp
  ${PREFLATE_DIR}/support/const_division.cpp
  ${PREFLATE_DIR}/support/huffman_decoder.cpp
  ${PREFLATE_DIR}/support/huffman_encoder.cpp
  ${PREFLATE_DIR}/support/huffman_helper.cpp
  ${PREFLATE_DIR}/support/memstream.cpp
  ${PREFLATE_DIR}/support/outputcachestream.cpp
  ${PREFLATE_DIR}/support/task_pool.cpp
)
add_library(preflate_vendor STATIC ${PREFLATE_SRC})
target_include_directories(preflate_vendor PUBLIC ${PREFLATE_DIR} ${PREFLATE_DIR}/support)
# Preflate is C++14-era code: it uses std::result_of, which C++20 removed. Build
# just this target as C++17 rather than patching vendored source.
set_target_properties(preflate_vendor PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# Our wrapper includes preflate's headers, which are C++14-era, so it is built as
# C++17 in its own target. The rest of the engine stays C++20 and only sees our
# own header, which does not include theirs.
add_library(preflate_shim STATIC
  src/precompressors/preflate_wrap.cpp
  src/precompressors/jpeg_wrap.cpp
  src/entropy/cm_zpaq.cpp)
set_target_properties(preflate_shim PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(preflate_shim PUBLIC preflate_vendor zpaq_vendor brunsli_vendor)
target_include_directories(preflate_shim PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Vendored third-party code: keep its warnings out of our build log.
if(NOT MSVC)
  target_compile_options(preflate_vendor PRIVATE -w)
endif()


# ---------------------------------------------------------------------------
# libbsc (Phase 9) — BWT backend, Apache-2.0.
#
# Compiled directly, like preflate: its CMakeLists prefers clang and enables
# OpenMP by default, neither of which we want. The file list is the one its own
# build declares.
# ---------------------------------------------------------------------------
set(LIBBSC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/libbsc-master)
add_library(bsc_vendor STATIC
  ${LIBBSC_DIR}/libbsc/adler32/adler32.cpp
  ${LIBBSC_DIR}/libbsc/bwt/bwt.cpp
  ${LIBBSC_DIR}/libbsc/bwt/libsais/libsais.c
  ${LIBBSC_DIR}/libbsc/coder/coder.cpp
  ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc.cpp
  ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc_model.cpp
  ${LIBBSC_DIR}/libbsc/filters/detectors.cpp
  ${LIBBSC_DIR}/libbsc/filters/preprocessing.cpp
  ${LIBBSC_DIR}/libbsc/libbsc/libbsc.cpp
  ${LIBBSC_DIR}/libbsc/lzp/lzp.cpp
  ${LIBBSC_DIR}/libbsc/platform/platform.cpp
  ${LIBBSC_DIR}/libbsc/st/st.cpp
)
target_include_directories(bsc_vendor PUBLIC ${LIBBSC_DIR})
if(NOT MSVC)
  target_compile_options(bsc_vendor PRIVATE -w)
endif()


# ---------------------------------------------------------------------------
# libzpaq (Phase 10) — context-mixing backend for --max-ratio, public domain.
# One translation unit; built as C++17 because the source predates C++20.
# ---------------------------------------------------------------------------
set(ZPAQ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/zpaq-master)
add_library(zpaq_vendor STATIC ${ZPAQ_DIR}/libzpaq.cpp)
target_include_directories(zpaq_vendor PUBLIC ${ZPAQ_DIR})
set_target_properties(zpaq_vendor PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# libzpaq branches on `#ifdef unix` and otherwise includes <windows.h>. The macro
# is not predefined by this compiler, so it has to be given explicitly. NOJIT
# avoids the x86 JIT, which wants executable memory we should not depend on.
target_compile_definitions(zpaq_vendor PUBLIC unix NOJIT)
if(NOT MSVC)
  target_compile_options(zpaq_vendor PRIVATE -w)
endif()


# ---------------------------------------------------------------------------
# PPMd (Phase 11) — text backend from the 7-Zip SDK, public domain.
# Three C files plus the range coder that lives inside them.
# ---------------------------------------------------------------------------
set(PPMD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/7z/C)
add_library(ppmd_vendor STATIC
  ${PPMD_DIR}/Ppmd7.c
  ${PPMD_DIR}/Ppmd7Enc.c
  ${PPMD_DIR}/Ppmd7Dec.c
)
target_include_directories(ppmd_vendor PUBLIC ${PPMD_DIR})
if(NOT MSVC)
  target_compile_options(ppmd_vendor PRIVATE -w)
endif()

find_package(Threads REQUIRED)


# ---------------------------------------------------------------------------
# brunsli (Phase 18) — JPEG recompression, MIT (Google), vendored inside precomp.
# Depends on brotli, also vendored there. brotli's sources are C and must be
# compiled as C; passing them to the C++ driver fails with "-std=c++17 not
# allowed with C".
# ---------------------------------------------------------------------------
set(CONTRIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/precomp-cpp-master/contrib)
file(GLOB BROTLI_SRC ${CONTRIB_DIR}/brotli/c/*/*.c)
add_library(brotli_vendor STATIC ${BROTLI_SRC})
target_include_directories(brotli_vendor PUBLIC
  ${CONTRIB_DIR}/brotli/c/include ${CONTRIB_DIR}/brotli/c/common)
if(NOT MSVC)
  target_compile_options(brotli_vendor PRIVATE -w)
endif()

file(GLOB BRUNSLI_SRC ${CONTRIB_DIR}/brunsli/c/*/*.cc)
add_library(brunsli_vendor STATIC ${BRUNSLI_SRC})
set_target_properties(brunsli_vendor PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(brunsli_vendor PUBLIC
  ${CONTRIB_DIR}/brunsli/c/include ${CONTRIB_DIR}/brunsli/c/common
  ${CONTRIB_DIR}/brunsli/c/enc ${CONTRIB_DIR}/brunsli/c/dec)
target_link_libraries(brunsli_vendor PUBLIC brotli_vendor)
if(NOT MSVC)
  target_compile_options(brunsli_vendor PRIVATE -w)
endif()


# ---------------------------------------------------------------------------
# Plugins (Phase 19) — copyleft backends as separate shared objects.
#
# bzip3 is LGPL-3. LGPL allows an MIT program to use it *if the user can replace
# the library*, which a separate .so satisfies and static linking would not. So
# nothing LGPL is linked into the main binary: the plugin is built on its own and
# loaded at runtime. GPL backends (ARC, paq8px, lzo) are NOT offered this way —
# dynamic linking does not escape the GPL.
# ---------------------------------------------------------------------------
option(CHCE_BUILD_PLUGINS "Build the LGPL backend plugins" ON)
if(CHCE_BUILD_PLUGINS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/bzip3/src/libbz3.c)
  add_library(bz3_vendor STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/bzip3/src/libbz3.c)
  target_include_directories(bz3_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hcs/old/bzip3/include)
  # libbz3.c reports its version and has no fallback define; without this it does
  # not compile at all.
  target_compile_definitions(bz3_vendor PRIVATE VERSION=\"1.5.1\")
  if(NOT MSVC)
    target_compile_options(bz3_vendor PRIVATE -w)
  endif()

  add_library(bzip3_plugin SHARED plugins/bzip3_plugin.cpp)
  target_include_directories(bzip3_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(bzip3_plugin PRIVATE bz3_vendor)
  set_target_properties(bzip3_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME bzip3)
endif()

add_library(chce_core STATIC
  src/engine/block_writer.cpp
  src/engine/plugin_host.cpp
  src/engine/compressor.cpp
  src/engine/decompressor.cpp
  src/engine/cost_model.cpp
  src/engine/match_finder.cpp
  src/engine/parser.cpp
  src/engine/token_stream.cpp
  src/entropy/bwt_bsc.cpp
  src/entropy/ppmd.cpp
  src/entropy/huffman.cpp
  src/entropy/rans.cpp
  src/entropy/range_coder.cpp
  src/precompressors/bcj_x86.cpp
  src/precompressors/gif_wrap.cpp
  src/precompressors/numeric_wrap.cpp
  src/precompressors/xml_numeric_wrap.cpp
  src/precompressors/dedup.cpp
  src/dedup/coarse_index.cpp
  src/entropy/token_coder.cpp
  src/codecs/lzma2_fallback.cpp
)

target_include_directories(chce_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(chce_core PRIVATE liblzma preflate_shim bsc_vendor zpaq_vendor ppmd_vendor brunsli_vendor Threads::Threads ${CMAKE_DL_LIBS})
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
