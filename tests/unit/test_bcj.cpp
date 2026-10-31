// test_bcj.cpp — the x86 BCJ transform must be exactly invertible on anything.
#include <cstring>
#include <vector>

#include "precompressors/bcj_x86.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

// Synthetic x86-ish stream: CALL/JMP opcodes with relative displacements.
std::vector<uint8_t> x86_like(size_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d;
  d.reserve(n);
  while (d.size() < n) {
    const uint8_t op = rng.below(10) == 0 ? 0xE8 : (rng.below(3) == 0 ? 0xE9 : rng.byte());
    d.push_back(op);
    if (op == 0xE8 || op == 0xE9) {
      // A plausible relative displacement, with the sign byte 00/FF.
      const uint32_t v = static_cast<uint32_t>(rng.next() % 0xFFFF);
      d.push_back(static_cast<uint8_t>(v));
      d.push_back(static_cast<uint8_t>(v >> 8));
      d.push_back(static_cast<uint8_t>(v >> 16));
      d.push_back(rng.below(2) ? 0x00 : 0xFF);
    }
  }
  return d;
}

}  // namespace

TEST(round_trip_on_random_data) {
  for (uint64_t seed : {1ull, 2ull, 3ull}) {
    auto data = x86_like(200000, seed);
    auto t = data;
    bcj_x86_transform(t, true);
    bcj_x86_transform(t, false);
    CHECK_MSG(t == data, "BCJ is not invertible on x86-like data, seed " +
                             std::to_string(seed));
  }
}

TEST(round_trip_on_arbitrary_bytes) {
  // The transform must be an involution-pair for *any* input, not just code.
  for (uint64_t seed : {11ull, 12ull}) {
    chce_test::Rng rng(seed);
    std::vector<uint8_t> data(50000);
    for (auto& b : data) b = rng.byte();
    auto t = data;
    bcj_x86_transform(t, true);
    bcj_x86_transform(t, false);
    CHECK_MSG(t == data, "BCJ is not invertible on random bytes");
  }
  // Degenerate sizes.
  for (size_t n : {size_t{0}, size_t{1}, size_t{4}, size_t{5}, size_t{6}, size_t{17}}) {
    std::vector<uint8_t> data(n, 0xE8);
    auto t = data;
    bcj_x86_transform(t, true);
    bcj_x86_transform(t, false);
    CHECK_MSG(t == data, "BCJ failed at size " + std::to_string(n));
  }
}

TEST(actually_transforms_code) {
  // If nothing changed, the filter is not doing its job.
  auto data = x86_like(100000, 7);
  auto t = data;
  bcj_x86_transform(t, true);
  CHECK_MSG(t != data, "transform was a no-op on x86-like data");
}

TEST(detection) {
  auto code = x86_like(100000, 5);
  CHECK(bcj_x86_likely(code));

  chce_test::Rng rng(9);
  std::vector<uint8_t> text(100000);
  for (auto& b : text) b = static_cast<uint8_t>('a' + rng.below(26));
  CHECK_MSG(!bcj_x86_likely(text), "text was misdetected as x86 code");

  std::vector<uint8_t> tiny(100, 0xE8);
  CHECK_MSG(!bcj_x86_likely(tiny), "a tiny buffer should not be classified");
}

TEST_MAIN()
