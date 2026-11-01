// test_numeric.cpp — the integer column transform must be exact or decline.
//
// The contract: every accepted file round-trips byte-for-byte, and every file that
// cannot be reproduced exactly is declined rather than normalised. A CSV with
// "007" must not come back as "7".
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/numeric_wrap.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string str(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

std::string make_csv(size_t rows, bool header, const char* eol = "\n") {
  std::string s;
  if (header) { s += "id,value,count"; s += eol; }
  long a = 1000, b = 500000, c = 0;
  for (size_t i = 0; i < rows; ++i) {
    a += 3;
    b += static_cast<long>(i % 7);
    c += 1;
    s += std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c) + eol;
  }
  return s;
}
}  // namespace

TEST(round_trips_integer_csv_with_header) {
  const std::string text = make_csv(500, true);
  std::vector<uint8_t> packed;
  CHECK_MSG(numeric_pack(bytes(text), packed), "declined a clean integer CSV");
  CHECK_MSG(packed.size() < text.size(), "transform did not shrink the data");

  std::vector<uint8_t> back;
  CHECK_MSG(numeric_unpack(packed, back), "unpack failed");
  CHECK_MSG(str(back) == text, "round-trip is not byte-exact");
}

TEST(round_trips_crlf) {
  const std::string text = make_csv(200, true, "\r\n");
  std::vector<uint8_t> packed;
  if (!numeric_pack(bytes(text), packed)) return;
  std::vector<uint8_t> back;
  CHECK_MSG(numeric_unpack(packed, back), "unpack failed");
  CHECK_MSG(str(back) == text, "CRLF round-trip is not byte-exact");
}

TEST(round_trips_tsv) {
  std::string text;
  long a = 5, b = 900;
  for (int i = 0; i < 300; ++i) {
    a += 2;
    b += 11;
    text += std::to_string(a) + "\t" + std::to_string(b) + "\n";
  }
  std::vector<uint8_t> packed;
  CHECK_MSG(numeric_pack(bytes(text), packed), "declined a clean TSV");
  std::vector<uint8_t> back;
  CHECK_MSG(numeric_unpack(packed, back), "unpack failed");
  CHECK_MSG(str(back) == text, "TSV round-trip is not byte-exact");
}

TEST(declines_quoted_csv) {
  // RFC 4180 quoting is not handled in v1: decline, do not guess.
  const std::string text = "a,b\n\"1,2\",3\n4,5\n6,7\n";
  std::vector<uint8_t> packed;
  CHECK_MSG(!numeric_pack(bytes(text), packed), "accepted a quoted CSV");
}

TEST(declines_non_canonical_integers) {
  // Leading zeros cannot round-trip through an integer, so the column must be
  // declined rather than normalised to 7, 8, 9.
  const std::string text = "a,b\n007,1\n008,2\n009,3\n";
  std::vector<uint8_t> packed;
  const bool packed_ok = numeric_pack(bytes(text), packed);
  if (packed_ok) {
    // If it accepted, column "a" must have been kept as raw text.
    std::vector<uint8_t> back;
    CHECK_MSG(numeric_unpack(packed, back), "unpack failed");
    CHECK_MSG(str(back) == text, "normalised a non-canonical column");
  }
}

TEST(declines_plain_text) {
  std::string text;
  for (int i = 0; i < 400; ++i) text += "the quick brown fox jumps over the lazy dog\n";
  std::vector<uint8_t> packed;
  CHECK_MSG(!numeric_pack(bytes(text), packed), "accepted prose as numeric data");
}

TEST_MAIN()
