// parse_stats.cpp — Phase 3 measurement: how good is the parse, on its own?
//
// The LZ_RAW block body stores literals as raw bytes, so its size measures the
// *absence of an entropy coder*, not the quality of the parse. This tool
// separates the two: it reports the modelled cost of the parse (ideal bits
// under the same order-1 literal model an adaptive coder converges to), next
// to what the transitional encoding actually costs.
//
// If the modelled cost is close to LZMA2's real output, the parse is not the
// problem — the missing coder is.
//
// usage: parse_stats <file>...
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chimera/cost_model.hpp"
#include "chimera/match_finder.hpp"
#include "engine/parser.hpp"

namespace {

std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path, "rb");
  if (!f) return data;
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  return data;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: parse_stats <file>...\n");
    return 2;
  }

  std::printf("%-12s %10s %8s %8s %8s %10s %9s %8s\n", "file", "bytes",
              "lit%", "match%", "avgLen", "ideal MiB", "LZ_RAW", "MB/s");
  std::printf("%s\n", std::string(84, '-').c_str());

  uint64_t t_bytes = 0, t_lits = 0, t_matches = 0, t_matched_bytes = 0;
  double t_ideal_bits = 0;

  for (int a = 1; a < argc; ++a) {
    auto data = read_file(argv[a]);
    if (data.empty()) {
      std::fprintf(stderr, "skipping %s\n", argv[a]);
      continue;
    }

    const auto t0 = std::chrono::steady_clock::now();

    chce::StaticCostModel model(data);
    chce::MatchFinder::Options mf_opt;
    mf_opt.window = 64u << 20;
    mf_opt.nice_len = chce::kMaxMatchLen;
    chce::MatchFinder finder(data, mf_opt);

    const uint32_t bs = 256 * 1024;
    std::vector<chce::Token> tokens;
    uint64_t lits = 0, matches = 0, matched_bytes = 0;
    double ideal_bits = 0;
    uint64_t raw_body = 0;  // size the LZ_RAW encoding would take

    for (uint32_t pos = 0; pos < data.size(); pos += bs) {
      const uint32_t end =
          std::min<uint32_t>(pos + bs, static_cast<uint32_t>(data.size()));
      const uint64_t cost =
          chce::parse_block(data, pos, end, finder, model, tokens);
      ideal_bits += static_cast<double>(cost) / 16.0;

      uint64_t block_lits = 0;
      for (const auto& t : tokens) {
        if (t.is_literal()) {
          ++lits;
          ++block_lits;
        } else {
          ++matches;
          matched_bytes += t.len;
          // tag + two varints, roughly: 1 + len bytes + dist bytes
          raw_body += 1 + 2 + 3;
        }
      }
      raw_body += block_lits;  // one byte per literal (this block's, not the running total)
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mbs = secs > 0 ? data.size() / secs / 1e6 : 0.0;

    char name[13];
    std::snprintf(name, sizeof(name), "%s", argv[a]);
    const char* base = std::strrchr(name, '/');
    const double total_syms = static_cast<double>(lits + matches);
    std::printf("%-12s %10zu %7.1f%% %7.1f%% %8.1f %10.2f %9llu %8.2f\n",
                base ? base + 1 : name, data.size(),
                total_syms ? 100.0 * lits / total_syms : 0.0,
                total_syms ? 100.0 * matches / total_syms : 0.0,
                matches ? static_cast<double>(matched_bytes) / matches : 0.0,
                ideal_bits / 8.0 / (1024.0 * 1024.0),
                static_cast<unsigned long long>(raw_body), mbs);

    t_bytes += data.size();
    t_lits += lits;
    t_matches += matches;
    t_matched_bytes += matched_bytes;
    t_ideal_bits += ideal_bits;
  }

  std::printf("%s\n", std::string(84, '-').c_str());
  std::printf("TOTAL bytes=%llu literals=%llu matches=%llu matched_bytes=%llu\n",
              static_cast<unsigned long long>(t_bytes),
              static_cast<unsigned long long>(t_lits),
              static_cast<unsigned long long>(t_matches),
              static_cast<unsigned long long>(t_matched_bytes));
  std::printf("      modelled cost of the parse = %.2f MiB of ideal bits\n",
              t_ideal_bits / 8.0 / (1024.0 * 1024.0));
  return 0;
}
