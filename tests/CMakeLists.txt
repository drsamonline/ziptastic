# Test suite. Each unit test is its own executable with TEST_MAIN() inside,
# registered with CTest. Integration tests are shell scripts driving the CLI.

set(CHCE_UNIT_TESTS varint header lzma2 match_finder parser rans range_coder bcj coarse_index huffman roundtrip fuzz)

foreach(t ${CHCE_UNIT_TESTS})
  add_executable(test_${t} unit/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE chce_core)
  target_include_directories(test_${t} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME unit.${t} COMMAND test_${t})
endforeach()

if(UNIX)
  add_test(NAME integration.cli
    COMMAND ${CMAKE_COMMAND} -E env
            ZIPT_BIN=$<TARGET_FILE:ziptastic>
            bash ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_cli.sh)
endif()

# Benchmark harnesses are built but not registered with ctest: they need a
# corpus and they take minutes.
add_executable(match_stats benchmark/match_stats.cpp)
target_link_libraries(match_stats PRIVATE chce_core)
target_include_directories(match_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(parse_stats benchmark/parse_stats.cpp)
target_link_libraries(parse_stats PRIVATE chce_core)
target_include_directories(parse_stats PRIVATE ${CMAKE_SOURCE_DIR}/src)
