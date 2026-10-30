// test_fuzz.cpp — 1000 mutated streams must not crash or hang.
//
// Every mutation either decodes to something (allowed: the decoder is not a
// MAC) or returns an error code. What must never happen is a crash, a hang,
// or a successful decode whose output disagrees with its own header.
#include <cstring>

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> sample_stream() {
  // A multi-block stream with varied content.
  chce_test::Rng rng(1234);
  std::vector<uint8_t> data(9000);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>((i % 97) ^ rng.byte());
  }
  CompressOptions opt;
  opt.block_size = 1024;
  std::vector<uint8_t> out;
  compress(data, out, opt, nullptr);
  return out;
}

enum class Mutation { FlipBit, SetByte, Insert, Delete, Truncate, ZeroRun };

void apply(std::vector<uint8_t>& buf, chce_test::Rng& rng, Mutation kind) {
  if (buf.empty()) return;
  switch (kind) {
    case Mutation::FlipBit: {
      const size_t i = static_cast<size_t>(rng.below(buf.size()));
      buf[i] ^= static_cast<uint8_t>(1u << rng.below(8));
      break;
    }
    case Mutation::SetByte: {
      const size_t i = static_cast<size_t>(rng.below(buf.size()));
      buf[i] = rng.byte();
      break;
    }
    case Mutation::Insert: {
      const size_t i = static_cast<size_t>(rng.below(buf.size() + 1));
      buf.insert(buf.begin() + static_cast<long>(i), rng.byte());
      break;
    }
    case Mutation::Delete: {
      const size_t i = static_cast<size_t>(rng.below(buf.size()));
      buf.erase(buf.begin() + static_cast<long>(i));
      break;
    }
    case Mutation::Truncate: {
      buf.resize(static_cast<size_t>(rng.below(buf.size())));
      break;
    }
    case Mutation::ZeroRun: {
      const size_t i = static_cast<size_t>(rng.below(buf.size()));
      const size_t n = static_cast<size_t>(rng.below(16)) + 1;
      for (size_t k = i; k < buf.size() && k < i + n; ++k) buf[k] = 0;
      break;
    }
  }
}

// Decodes and asserts only the invariants that must hold for ANY input.
void decode_and_check(const std::vector<uint8_t>& buf) {
  std::vector<uint8_t> out;
  DecompressReport rep;
  const Err e = decompress(buf, out, &rep);
  if (e == Err::Ok) {
    CHECK_EQ(out.size(), rep.original_size);
  } else {
    CHECK_MSG(out.empty() || out.size() <= kMaxStreamSize, "implausible output");
  }
}

}  // namespace

TEST(mutations_do_not_crash) {
  const auto base = sample_stream();
  chce_test::Rng rng(2024);
  const Mutation kinds[] = {Mutation::FlipBit, Mutation::SetByte, Mutation::Insert,
                            Mutation::Delete,  Mutation::Truncate, Mutation::ZeroRun};

  for (int i = 0; i < 1000; ++i) {
    auto buf = base;
    const int rounds = 1 + static_cast<int>(rng.below(4));
    for (int k = 0; k < rounds; ++k) {
      apply(buf, rng, kinds[rng.below(6)]);
    }
    decode_and_check(buf);
  }
}

TEST(header_field_attacks) {
  const auto base = sample_stream();
  chce_test::Rng rng(777);

  // target: 4 magic + version + flags + original_size + block_count
  for (int i = 0; i < 400; ++i) {
    auto buf = base;
    const size_t field = static_cast<size_t>(rng.below(12));
    if (field < buf.size()) buf[field] = rng.byte();
    decode_and_check(buf);
  }

  // Absurd declared sizes and block counts, encoded as valid varints.
  for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{1} << 20,
                     (uint64_t{1} << 40) - 1, uint64_t{1} << 40}) {
    for (uint64_t blocks : {uint64_t{0}, uint64_t{1}, uint64_t{1} << 20}) {
      ByteWriter w;
      w.bytes(kMagic, sizeof(kMagic));
      w.u8(kFormatVersion);
      w.u8(0);
      w.varint(v);
      w.varint(blocks);
      // Append the original payload so the block loop has real bytes to chew.
      w.bytes(base.data() + 8, base.size() - 8);
      decode_and_check(w.buffer());
    }
  }
}

TEST(garbage_inputs) {
  chce_test::Rng rng(31337);
  for (int i = 0; i < 200; ++i) {
    const size_t n = static_cast<size_t>(rng.below(256));
    std::vector<uint8_t> buf(n);
    for (auto& b : buf) b = rng.byte();
    // Half of them get a valid magic so the block parser is reached.
    if (i % 2 == 0 && n >= 4) std::memcpy(buf.data(), kMagic, 4);
    decode_and_check(buf);
  }
}

TEST_MAIN()
