// test_lzma2.cpp — the Tier-1 fallback codec, on its own.
#include <cstring>

#include "chimera/format.hpp"
#include "codecs/lzma2_fallback.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> text_data(size_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  static const char* words[] = {"the ", "quick ", "brown ", "fox ",
                                "jumps ", "over ", "lazy ", "dog. "};
  std::vector<uint8_t> d;
  d.reserve(n);
  while (d.size() < n) {
    const char* w = words[rng.below(8)];
    for (size_t i = 0; w[i] && d.size() < n; ++i) d.push_back(uint8_t(w[i]));
  }
  return d;
}

void roundtrip(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> payload, props, out;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);
  CHECK_EQ(props.size(), size_t{1});
  CHECK(!payload.empty());

  CHECK_ERR(lzma2_decode(payload, props, data.size(), out), Err::Ok);
  CHECK_EQ(out.size(), data.size());
  CHECK_MSG(out == data, "lzma2 round-trip mismatch at " +
                             std::to_string(data.size()) + " bytes");
}

}  // namespace

TEST(available) { CHECK(lzma2_available()); }

TEST(roundtrip_sizes) {
  for (size_t n : {size_t{1}, size_t{2}, size_t{17}, size_t{1000}, size_t{65536},
                   size_t{262144}, size_t{1} << 20}) {
    roundtrip(text_data(n, n));
  }
}

TEST(roundtrip_content_kinds) {
  chce_test::Rng rng(11);
  std::vector<uint8_t> noise(200000);
  for (auto& b : noise) b = rng.byte();
  roundtrip(noise);

  std::vector<uint8_t> zeros(200000, 0);
  roundtrip(zeros);

  std::vector<uint8_t> ramp(200000);
  for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = uint8_t(i);
  roundtrip(ramp);
}

TEST(compresses_text) {
  const auto data = text_data(1 << 20, 7);
  std::vector<uint8_t> payload, props;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);
  CHECK_MSG(payload.size() < data.size() / 2,
            "expected >2:1 on repetitive text, got " +
                std::to_string(payload.size()) + " from " +
                std::to_string(data.size()));
}

TEST(empty_input_rejected) {
  std::vector<uint8_t> payload, props;
  CHECK_ERR(lzma2_encode({}, payload, props), Err::BadBlockSize);
}

TEST(wrong_size_rejected) {
  const auto data = text_data(5000, 3);
  std::vector<uint8_t> payload, props, out;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);

  CHECK_ERR(lzma2_decode(payload, props, data.size() + 1, out), Err::Corrupt);
  CHECK_ERR(lzma2_decode(payload, props, data.size() - 1, out), Err::Corrupt);
  CHECK_ERR(lzma2_decode(payload, props, 0, out), Err::BadBlockSize);
}

TEST(corrupt_payload_rejected) {
  const auto data = text_data(20000, 5);
  std::vector<uint8_t> payload, props, out;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);

  // Flip a bit in the middle of the stream: must be an error, never a crash
  // and never silently wrong data.
  for (size_t at : {payload.size() / 4, payload.size() / 2, payload.size() - 1}) {
    auto bad = payload;
    bad[at] ^= 0x40;
    const Err e = lzma2_decode(bad, props, data.size(), out);
    if (e == Err::Ok) {
      // A flip in a length/control byte can still decode; if so it must not
      // claim to be the original data.
      CHECK_MSG(out != data, "corrupted stream decoded to identical output");
    }
  }
}

TEST(bad_props_rejected) {
  const auto data = text_data(4000, 9);
  std::vector<uint8_t> payload, props, out;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);

  CHECK_ERR(lzma2_decode(payload, {}, data.size(), out), Err::BadMode);
  std::vector<uint8_t> wide(20, 0xFF);
  CHECK_ERR(lzma2_decode(payload, wide, data.size(), out), Err::BadMode);

  // Dictionary-size byte outside the legal range.
  std::vector<uint8_t> wrong_props = {0x7F};
  CHECK_ERR(lzma2_decode(payload, wrong_props, data.size(), out), Err::BadMode);
}

TEST(truncated_payload_rejected) {
  const auto data = text_data(30000, 13);
  std::vector<uint8_t> payload, props, out;
  CHECK_ERR(lzma2_encode(data, payload, props), Err::Ok);

  for (size_t cut : {size_t{1}, payload.size() / 2, payload.size() - 1}) {
    std::span<const uint8_t> part(payload.data(), cut);
    CHECK_ERR(lzma2_decode(part, props, data.size(), out), Err::Corrupt);
  }
}

TEST_MAIN()
