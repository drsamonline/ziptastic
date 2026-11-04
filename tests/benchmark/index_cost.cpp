// index_cost.cpp — what the v8 offset table costs, and what it buys.
//
// The table is eight bytes a block, so its cost is arithmetic. What is worth measuring is
// the other side: how much of a stream a reader has to touch to reach one block, and how
// long that takes against decoding the whole thing.
//
//   index_cost <file> [block_kib]
//
// The stream is built with the Layer 0 and stream-level transforms off and --parallel on,
// because that is the shape a block can be decoded from on its own. Compression itself is
// left on: the blocks are real PPMd/rANS blocks, not stored ones.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "chimera/block_index.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"

using namespace chce;
using Clock = std::chrono::steady_clock;

namespace {

bool read_all(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t chunk[1 << 16];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    out.insert(out.end(), chunk, chunk + n);
  }
  std::fclose(f);
  return true;
}

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

CompressOptions options(size_t block_size) {
  CompressOptions opt;
  opt.block_size = block_size;
  opt.parallel = true;
  opt.threads = 2;
  opt.lzma2_fallback = false;
  opt.max_ratio = false;
  opt.precomp = false;
  opt.jpeg = false;
  opt.gif = false;
  opt.numeric = false;
  opt.base64 = false;
  opt.wav = false;
  opt.sqlite = false;
  opt.ecc = false;
  opt.bmp = false;
  opt.dedup = false;
  opt.bcj = false;
  opt.coarse = false;
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: index_cost <file> [block_kib]\n");
    return 2;
  }
  const size_t block_size =
      (argc > 2 ? static_cast<size_t>(std::atoi(argv[2])) : 64) * 1024;

  std::vector<uint8_t> input;
  if (!read_all(argv[1], input) || input.empty()) {
    std::fprintf(stderr, "index_cost: cannot read %s\n", argv[1]);
    return 1;
  }

  std::vector<uint8_t> packed;
  const auto c0 = Clock::now();
  if (const Err e = compress(input, packed, options(block_size), nullptr); e != Err::Ok) {
    std::fprintf(stderr, "index_cost: compress failed: %s\n", err_message(e));
    return 1;
  }
  const auto c1 = Clock::now();

  // The index, five times, smallest: it is microseconds and the first run pays for cold
  // pages.
  StreamIndex idx;
  double best_index = 1e9;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = Clock::now();
    if (const Err e = read_index(packed, idx); e != Err::Ok) {
      std::fprintf(stderr, "index_cost: read_index failed: %s\n", err_message(e));
      return 1;
    }
    const auto t1 = Clock::now();
    best_index = std::min(best_index, ms(t0, t1));
  }
  if (idx.size() != idx.header.block_count || idx.size() == 0) {
    std::fprintf(stderr, "index_cost: the index does not describe the stream\n");
    return 1;
  }

  // The whole stream, the way a decoder without a table has to.
  std::vector<uint8_t> whole;
  const auto d0 = Clock::now();
  if (const Err e = decompress(packed, whole); e != Err::Ok) {
    std::fprintf(stderr, "index_cost: decompress failed: %s\n", err_message(e));
    return 1;
  }
  const auto d1 = Clock::now();
  if (whole != input) {
    std::fprintf(stderr, "index_cost: round trip mismatch\n");
    return 1;
  }

  // One block, from its offset. A middle block rather than the last, because the last
  // one of a file is usually short and would flatter the number.
  const uint64_t which = idx.size() / 2;
  std::vector<uint8_t> one;
  const auto b0 = Clock::now();
  if (const Err e = decode_blocks(packed, idx, which, which, one); e != Err::Ok) {
    std::fprintf(stderr, "index_cost: decode_blocks failed: %s\n", err_message(e));
    return 1;
  }
  const auto b1 = Clock::now();

  const double trailer = static_cast<double>(trailer_bytes(idx.size()));
  // What v7's trailer cost for the same block count: "ZTIX", the count varint, the
  // manifest, and nothing else.
  uint64_t count = idx.size();
  double count_bytes = 1;
  while (count >= 0x80) {
    count >>= 7;
    count_bytes += 1;
  }
  const double v7_trailer = 4.0 + count_bytes + 32.0;

  std::printf("input                  %zu bytes\n", input.size());
  std::printf("archive                %zu bytes (%.2f%%)\n", packed.size(),
              100.0 * static_cast<double>(packed.size()) / static_cast<double>(input.size()));
  std::printf("blocks                 %zu of %zu KiB\n", idx.size(), block_size / 1024);
  std::printf("trailer                %.0f bytes (v7 would be %.0f, so +%.0f = %.3f%% "
              "of the archive)\n",
              trailer, v7_trailer, trailer - v7_trailer,
              100.0 * (trailer - v7_trailer) / static_cast<double>(packed.size()));
  std::printf("compress               %.1f ms\n", ms(c0, c1));
  std::printf("read_index             %.3f ms, %llu of %zu bytes touched (%.3f%%)\n",
              best_index, static_cast<unsigned long long>(idx.bytes_read), packed.size(),
              100.0 * static_cast<double>(idx.bytes_read) /
                  static_cast<double>(packed.size()));
  std::printf("decompress (all)       %.1f ms, %zu bytes out\n", ms(d0, d1), whole.size());
  std::printf("decode one block       %.1f ms, %zu bytes out (block %llu of %zu)\n",
              ms(b0, b1), one.size(), static_cast<unsigned long long>(which), idx.size());
  std::printf("ratio                  %.1fx\n", ms(d0, d1) / std::max(ms(b0, b1), 0.001));
  return 0;
}
