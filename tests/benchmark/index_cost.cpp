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

  // The whole stream, the way a decoder without a table has to. Timed twice and the
  // smaller kept, exactly like the index above and the block below: a comparison where one
  // side gets two runs and the other one is not a comparison.
  std::vector<uint8_t> whole;
  double walk = 1e9;
  for (int i = 0; i < 2; ++i) {
    whole.clear();
    const auto d0 = Clock::now();
    if (const Err e = decompress(packed, whole); e != Err::Ok) {
      std::fprintf(stderr, "index_cost: decompress failed: %s\n", err_message(e));
      return 1;
    }
    const auto d1 = Clock::now();
    walk = std::min(walk, ms(d0, d1));
  }
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

  // The fourth feature the release plan asked for: parallel decode without a sequential
  // scan. `decompress()` walks every block header from the front before it hands anything
  // to a thread; this asks the index where the blocks are instead. Both are timed, because
  // the honest question is not "is it possible" but "is it faster".
  auto index_parallel = [&](unsigned nt) {
    StreamIndex local;
    if (read_index(packed, local) != Err::Ok) return std::vector<uint8_t>{};
    std::vector<uint8_t> joined;
    if (decode_blocks_parallel(packed, local, nt, joined) != Err::Ok) return std::vector<uint8_t>{};
    return joined;
  };

  // Warm both paths before timing either. The first decode of the pair pays for the pages of
  // `packed` and for the first touch of the output buffer, and whichever runs first absorbs
  // all of it — which is how a 4-6% "win" appeared between two paths whose real difference is
  // milliseconds. A benchmark that times one thing before another has to pay for both first.
  {
    std::vector<uint8_t> warm;
    (void)decompress(packed, warm);
    (void)index_parallel(2);
  }

  double best_par = 1e9;
  std::vector<uint8_t> par;
  for (int i = 0; i < 2; ++i) {
    const auto p0 = Clock::now();
    par = index_parallel(2);
    const auto p1 = Clock::now();
    best_par = std::min(best_par, ms(p0, p1));
  }
  const bool par_ok = par == input;

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
  // Re-time the walk after the warm-up: the first walk above ran before the pages were hot,
  // and keeping that number would make the comparison unfair in the other direction.
  for (int i = 0; i < 2; ++i) {
    whole.clear();
    const auto d2 = Clock::now();
    if (decompress(packed, whole) != Err::Ok) return 1;
    const auto d3 = Clock::now();
    walk = std::min(walk, ms(d2, d3));
  }
  std::printf("decompress (all)       %.1f ms, %zu bytes out\n", walk, whole.size());
  std::printf("decode one block       %.1f ms, %zu bytes out (block %llu of %zu)\n",
              ms(b0, b1), one.size(), static_cast<unsigned long long>(which), idx.size());
  std::printf("ratio                  %.1fx\n", walk / std::max(ms(b0, b1), 0.001));
  std::printf("decode (index, 2 thr)  %.1f ms, %s\n", best_par,
              par_ok ? "identical to the original" : "MISMATCH");
  return 0;
}
