// test_sqlite.cpp — page grouping must be exact or declined.
//
// The fixture is a synthetic database rather than a real one: the transform only
// cares about the format's shape — the 16-byte magic, the page size at offset 16,
// and the B-tree type byte at the head of each page — so building one by hand keeps
// the test independent of any SQLite version and of a binary fixture file.
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/sqlite_wrap.hpp"

using namespace chce;

namespace {

constexpr size_t kPage = 512;

// A database of `pages` pages whose type bytes are the ones given, in that order.
std::vector<uint8_t> make_db(const std::vector<uint8_t>& types) {
  std::vector<uint8_t> db(types.size() * kPage, 0);
  const char* magic = "SQLite format 3";
  std::memcpy(db.data(), magic, 15);
  db[15] = 0;
  db[16] = static_cast<uint8_t>(kPage >> 8);  // page size, big-endian
  db[17] = static_cast<uint8_t>(kPage & 0xFF);
  for (size_t i = 0; i < types.size(); ++i) {
    uint8_t* page = db.data() + i * kPage;
    if (i == 0) {
      page[100] = types[i];  // page 1 carries the header first
    } else {
      page[0] = types[i];
    }
    // Give each page distinct content so a misplaced page cannot round-trip. Page 1
    // is skipped over its 100-byte header — the first version of this test wrote
    // there and destroyed the magic, which made the transform decline a fixture
    // that was supposed to be valid.
    const size_t from = (i == 0) ? 110 : 8;
    for (size_t k = from; k < from + 56; ++k) page[k] = static_cast<uint8_t>(i * 31 + k);
  }
  return db;
}

}  // namespace

TEST(round_trips_a_database) {
  // Interleaved on purpose: grouping only has work to do when types are mixed.
  const auto db = make_db({13, 13, 10, 13, 5, 10, 13, 2, 13, 13, 10, 5});
  std::vector<uint8_t> packed, back;
  CHECK_MSG(sqlite_pack(db, packed), "declined a synthetic database");
  CHECK_MSG(sqlite_unpack(packed, back), "unpack failed");
  CHECK_MSG(back == db, "round-trip is not byte-exact");
}

TEST(grouping_actually_reorders) {
  const auto db = make_db({13, 10, 5, 13, 10, 2});
  std::vector<uint8_t> packed;
  CHECK_MSG(sqlite_pack(db, packed), "declined a synthetic database");
  // The first packed page must be an interior-index page (type 2), because types
  // are emitted in ascending order and only the last input page is a type 2.
  CHECK_MSG(packed[0] == 2, "packed pages are not grouped by type");
}

TEST(declines_a_partial_page) {
  auto db = make_db({13, 13, 13});
  db.resize(db.size() - 100);  // a hot journal, not a database
  std::vector<uint8_t> packed;
  CHECK_MSG(!sqlite_pack(db, packed), "accepted a file with a partial page");
}

TEST(declines_non_sqlite) {
  const std::string prose =
      "SQLite format 3 is mentioned here, but this is a paragraph of text and not a "
      "database of any kind whatsoever, so there is nothing to group.";
  std::vector<uint8_t> packed;
  CHECK_MSG(!sqlite_pack({reinterpret_cast<const uint8_t*>(prose.data()), prose.size()},
                         packed),
            "accepted prose as a database");
}

TEST(unpack_rejects_a_damaged_payload) {
  const auto db = make_db({13, 10, 13, 5});
  std::vector<uint8_t> packed, back;
  CHECK_MSG(sqlite_pack(db, packed), "declined a synthetic database");

  auto truncated = packed;
  truncated.resize(truncated.size() - 5);
  CHECK_MSG(!sqlite_unpack(truncated, back), "accepted a truncated payload");

  auto wrong_magic = packed;
  wrong_magic[wrong_magic.size() - 1] = 'X';
  CHECK_MSG(!sqlite_unpack(wrong_magic, back), "accepted a wrong magic");
}

TEST(packing_is_deterministic) {
  const auto db = make_db({13, 13, 10, 5, 2, 10});
  std::vector<uint8_t> a, b;
  CHECK_MSG(sqlite_pack(db, a), "declined a synthetic database");
  CHECK_MSG(sqlite_pack(db, b), "declined a synthetic database");
  CHECK_MSG(a == b, "packing the same input twice gave different bytes");
}

TEST_MAIN()
