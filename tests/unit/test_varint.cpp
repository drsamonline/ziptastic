// test_varint.cpp — LEB128 varint and the ByteWriter/ByteReader contracts.
#include "chimera/format.hpp"
#include "harness.hpp"

using namespace chce;

TEST(varint_roundtrip) {
  const uint64_t values[] = {0,
                             1,
                             2,
                             126,
                             127,
                             128,
                             129,
                             255,
                             256,
                             16383,
                             16384,
                             (1ull << 31) - 1,
                             1ull << 31,
                             (1ull << 32) - 1,
                             1ull << 32,
                             (1ull << 56) + 12345,
                             (1ull << 63) - 1,
                             1ull << 63,
                             ~0ull};
  for (uint64_t v : values) {
    ByteWriter w;
    w.varint(v);
    ByteReader r(w.span());
    uint64_t got = 0;
    CHECK(r.varint(got));
    CHECK_EQ(got, v);
    CHECK(r.at_end());
  }
}

TEST(varint_known_encodings) {
  // Fixed points of LEB128 that the format spec documents.
  ByteWriter w;
  w.varint(0);
  w.varint(127);
  w.varint(128);
  w.varint(300);
  const std::vector<uint8_t> expect = {0x00, 0x7F, 0x80, 0x01, 0xAC, 0x02};
  CHECK(w.buffer() == expect);
}

TEST(varint_truncated) {
  // Every strict prefix of a multi-byte varint must be rejected.
  ByteWriter w;
  w.varint(1ull << 40);
  const auto buf = w.buffer();
  CHECK(buf.size() > 1);
  for (size_t n = 0; n < buf.size(); ++n) {
    ByteReader r(std::span<const uint8_t>(buf.data(), n));
    uint64_t v = 0;
    CHECK(!r.varint(v));
  }
}

TEST(varint_overlong_rejected) {
  // 10 continuation bytes: wider than 64 bits.
  std::vector<uint8_t> over(11, 0x80);
  ByteReader r(over);
  uint64_t v = 0;
  CHECK(!r.varint(v));

  // 10 bytes ending in a value that overflows the top group.
  std::vector<uint8_t> big = {0x80, 0x80, 0x80, 0x80, 0x80,
                              0x80, 0x80, 0x80, 0x80, 0x02};
  ByteReader r2(big);
  CHECK(!r2.varint(v));
}

TEST(varint_max_accepted) {
  // 0xFFFFFFFFFFFFFFFF encodes in exactly 10 bytes and must decode.
  ByteWriter w;
  w.varint(~0ull);
  CHECK_EQ(w.buffer().size(), size_t{10});
  ByteReader r(w.span());
  uint64_t v = 0;
  CHECK(r.varint(v));
  CHECK_EQ(v, ~0ull);
}

TEST(reader_bounds) {
  const std::vector<uint8_t> data = {1, 2, 3};
  ByteReader r(data);

  uint8_t b = 0;
  CHECK(r.u8(b));
  CHECK_EQ(b, uint8_t{1});
  CHECK_EQ(r.remaining(), size_t{2});

  std::span<const uint8_t> s;
  CHECK(!r.take(3, s));  // only 2 left
  CHECK(r.take(2, s));
  CHECK_EQ(s.size(), size_t{2});
  CHECK(r.at_end());
  CHECK(!r.skip(1));
  CHECK(!r.u8(b));
}

TEST(mode_names) {
  CHECK(std::string_view(mode_name(Mode::Raw)) == "RAW");
  CHECK(std::string_view(mode_name(Mode::Lzma2Fallback)) == "LZMA2_FALLBACK");
  CHECK(std::string_view(mode_name(Mode::Ppmd)) == "PPMD");
  CHECK(mode_is_known(mode_byte(Mode::Ppmd)));
  CHECK(!mode_is_known(mode_byte(Mode::Plugin) + 1));
  CHECK_EQ(mode_byte(Mode::Raw), uint8_t{0x00});
  CHECK_EQ(mode_byte(Mode::CmZpaq), uint8_t{0x09});
  CHECK_EQ(mode_byte(Mode::Ppmd), uint8_t{0x0A});
  CHECK_EQ(mode_byte(Mode::LzRaw), uint8_t{0x0B});
}

TEST_MAIN()
