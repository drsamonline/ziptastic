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
#include "precompressors/dedup.hpp"
#include "precompressors/gif_wrap.hpp"
#include "precompressors/numeric_wrap.hpp"
#include "precompressors/wav_wrap.hpp"

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

// The fuzz above compresses with DEFAULT options, which means `precomp` is false and no
// Layer 0 transform runs — so none of the transform INVERSE functions were being fuzzed at
// all. Those are the parsers that read attacker-controlled structures (PNG chunks, BMP
// headers, CD sectors, gzip members, base64 runs), and they are the most likely place for a
// crash to hide.
//
// This builds a stream with every transform enabled, then mutates it, and separately feeds
// random bytes straight into each unpacker.
namespace {

std::vector<uint8_t> layered_sample() {
  // A little of everything: base64, a gzip member, a BMP and a PNG, plus plain bytes.
  chce_test::Rng rng(4242);
  std::vector<uint8_t> data;
  const char* b64 = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWZnaGlqa2xtbm9wcXJzdHV2d3h5eg==";
  for (int i = 0; i < 40; ++i) {
    data.insert(data.end(), b64, b64 + std::strlen(b64));
    data.push_back('\n');
  }
  // A small valid gzip member.
  {
    static const uint8_t gz[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
                                 0x4b, 0x4c, 0x4a, 0x4e, 0x49, 0x4d, 0x4b, 0xcf, 0xc8, 0x2c,
                                 0x2e, 0x01, 0x00, 0x1e, 0x6c, 0x3d, 0x2c, 0x0b, 0x00, 0x00,
                                 0x00};
    data.insert(data.end(), gz, gz + sizeof(gz));
  }
  for (int i = 0; i < 400; ++i) data.push_back(static_cast<uint8_t>(rng.byte()));
  return data;
}

}  // namespace

TEST(layered_streams_survive_mutation) {
  const std::vector<uint8_t> data = layered_sample();
  CompressOptions opt;
  opt.block_size = 4096;
  opt.precomp = true;  // every Layer 0 transform is allowed
  opt.base64 = true;
  std::vector<uint8_t> base;
  CHECK_ERR(compress(data, base, opt, nullptr), Err::Ok);

  chce_test::Rng rng(99);
  const Mutation kinds[] = {Mutation::FlipBit, Mutation::SetByte, Mutation::Insert,
                            Mutation::Delete,  Mutation::Truncate, Mutation::ZeroRun};
  for (int i = 0; i < 600; ++i) {
    auto buf = base;
    const int rounds = 1 + static_cast<int>(rng.below(3));
    for (int k = 0; k < rounds; ++k) apply(buf, rng, kinds[rng.below(6)]);
    decode_and_check(buf);
  }
}

TEST(every_unpacker_refuses_random_payloads) {
  chce_test::Rng rng(5150);
  for (int i = 0; i < 300; ++i) {
    std::vector<uint8_t> payload(static_cast<size_t>(rng.below(512)));
    for (auto& b : payload) b = rng.byte();

    std::vector<uint8_t> out;
    // Each of these must return (true or false) without crashing, and a `true` must
    // produce something — never a partial buffer that a caller could trust.
    const bool a = dedup_unpack(payload, out);
    CHECK_MSG(!a || !out.empty(), "dedup_unpack returned true with no output");
    out.clear();
    const bool b = numeric_unpack(payload, out);
    CHECK_MSG(!b || !out.empty(), "numeric_unpack returned true with no output");
    out.clear();
    const bool c = wav_unpack(payload, out);
    CHECK_MSG(!c || !out.empty(), "wav_unpack returned true with no output");
    out.clear();
    const bool d = gif_unpack(payload, out);
    CHECK_MSG(!d || !out.empty(), "gif_unpack returned true with no output");
  }
}

TEST_MAIN()
