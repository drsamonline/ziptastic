// match_stats.cpp — Phase 2 measurement harness.
//
// For each input file it reports what the fine match finder actually finds,
// and cross-checks it against an exhaustive scan on sampled positions. The
// oracle check is the point: it says whether the finder is leaving matches on
// the table, which is the property the Phase 3 parser depends on.
//
// usage: match_stats [--window KiB] [--samples N] <file>...
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "chimera/match_finder.hpp"

namespace {

std::vector<uint8_t> read_file(const char* path, bool& ok) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    ok = false;
    return data;
  }
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
  ok = std::ferror(f) == 0;
  std::fclose(f);
  return data;
}

uint32_t brute_best(const std::vector<uint8_t>& d, uint32_t pos, uint32_t window,
                    uint32_t max_len) {
  uint32_t best = 0;
  if (pos == 0) return 0;
  const uint32_t first = pos > window - 1 ? pos - (window - 1) : 0;
  for (uint32_t p = first; p < pos; ++p) {
    uint32_t len = 0;
    while (len < max_len && pos + len < d.size() && d[p + len] == d[pos + len]) ++len;
    if (len > best) best = len;
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t window_kib = 1024;
  uint32_t samples = 200;
  std::vector<std::string> files;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--window" && i + 1 < argc) {
      window_kib = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (a == "--samples" && i + 1 < argc) {
      samples = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      files.push_back(a);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: match_stats [--window KiB] [--samples N] <file>...\n");
    return 2;
  }

  std::printf("%-12s %10s %8s %8s %8s %8s %9s %8s\n", "file", "bytes",
              "match%", "cover%", "avgLen", "oracle", "MB/s", "mem MB");
  std::printf("%s\n", std::string(80, '-').c_str());

  uint64_t total_bytes = 0, total_covered = 0, total_matched_pos = 0, total_pos = 0;
  uint64_t oracle_checked = 0, oracle_mismatch = 0;

  for (const auto& path : files) {
    bool ok = true;
    auto data = read_file(path.c_str(), ok);
    if (!ok || data.empty()) {
      std::fprintf(stderr, "skipping %s\n", path.c_str());
      continue;
    }

    chce::MatchFinder::Options opt;
    opt.window = window_kib * 1024;
    opt.nice_len = 273;   // LZMA's maximum match length
    opt.depth = 0;        // LZMA default cutoff
    chce::MatchFinder mf(data, opt);

    const auto t0 = std::chrono::steady_clock::now();

    // A greedy walk: take the longest match at each position and jump over it.
    // It is not the Phase 3 parser, but it measures the finder on its own.
    uint64_t matched_positions = 0, covered = 0, match_len_sum = 0, matches = 0;
    uint32_t pos = 0;
    while (pos < data.size()) {
      auto ms = mf.query(pos);
      if (!ms.empty() && ms.back().len >= 3) {
        const uint32_t len = ms.back().len;
        ++matched_positions;
        covered += len;
        match_len_sum += len;
        ++matches;
        mf.skip_to(pos + len);
        pos += len;
      } else {
        ++pos;
      }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double mbs = secs > 0 ? data.size() / secs / 1e6 : 0.0;

    // Oracle: does the finder find what an exhaustive scan finds?
    uint64_t checked = 0, mismatched = 0;
    if (samples > 0) {
      const uint32_t probe_window = std::min<uint32_t>(mf.window_size(), 256u * 1024);
      chce::MatchFinder probe(data, [&] {
        chce::MatchFinder::Options o;
        o.window = probe_window;
        o.nice_len = 273;
        o.depth = 100000;
        return o;
      }());

      // Spread the samples across the file, skipping the first window.
      const uint32_t lo = probe_window;
      const uint32_t hi = static_cast<uint32_t>(data.size());
      if (hi > lo) {
        const uint32_t stride = std::max<uint32_t>(1, (hi - lo) / std::max(1u, samples));
        for (uint32_t p = lo; p < hi; p += stride) {
          auto ms = probe.query(p);
          const uint32_t found = ms.empty() ? 0 : ms.back().len;
          const uint32_t truth = brute_best(data, p, probe.window_size(), 273);
          ++checked;
          if (truth >= 4 && found != truth) {
            ++mismatched;
            if (mismatched <= 3) {
              std::fprintf(stderr,
                           "  MISMATCH %s pos %u: finder %u, brute force %u\n",
                           path.c_str(), p, found, truth);
            }
          }
        }
      }
    }
    oracle_checked += checked;
    oracle_mismatch += mismatched;

    char name[13];
    std::snprintf(name, sizeof(name), "%s", path.c_str());
    const char* base = std::strrchr(name, '/');
    std::printf("%-12s %10zu %7.1f%% %7.1f%% %8.1f %7llu/%-3llu %8.1f %8.1f\n",
                base ? base + 1 : name, data.size(),
                100.0 * static_cast<double>(matched_positions) / data.size(),
                100.0 * static_cast<double>(covered) / data.size(),
                matches ? static_cast<double>(match_len_sum) / matches : 0.0,
                static_cast<unsigned long long>(checked - mismatched),
                static_cast<unsigned long long>(checked), mbs,
                mf.memory_bytes() / (1024.0 * 1024.0));

    total_bytes += data.size();
    total_covered += covered;
    total_matched_pos += matched_positions;
    total_pos += data.size();
  }

  std::printf("%s\n", std::string(80, '-').c_str());
  std::printf("TOTAL bytes=%llu  greedy coverage=%.1f%%  matched positions=%.1f%%\n",
              static_cast<unsigned long long>(total_bytes),
              total_pos ? 100.0 * static_cast<double>(total_covered) / total_pos : 0.0,
              total_pos ? 100.0 * static_cast<double>(total_matched_pos) / total_pos : 0.0);
  std::printf("ORACLE %llu/%llu positions agree with an exhaustive scan\n",
              static_cast<unsigned long long>(oracle_checked - oracle_mismatch),
              static_cast<unsigned long long>(oracle_checked));
  return oracle_mismatch == 0 ? 0 : 1;
}
