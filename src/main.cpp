// main.cpp — ziptastic CLI.
//
//   ziptastic [command] [options] [input] [output]
//
// Commands: compress (default), decompress, bench, version, help.
// With no file arguments the tool filters stdin to stdout, so the Phase 0
// smoke test is exactly: echo hi | ziptastic | ziptastic -d
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "chimera/format.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersionString = "ziptastic 0.1.0-dev (CHCE format v1)";

void print_usage(FILE* f) {
  std::fprintf(f,
      "ziptastic — CHCE (Chimera Hybrid Compression Engine)\n"
      "\n"
      "usage: ziptastic [command] [options] [input] [output]\n"
      "\n"
      "commands:\n"
      "  compress, c        compress input to output (default)\n"
      "  decompress, d      decompress input to output\n"
      "  bench              compress and report ratio + speed, no output file\n"
      "  version            print version and exit\n"
      "  help               print this text and exit\n"
      "\n"
      "options:\n"
      "  -d, --decompress     same as the decompress command\n"
      "  -z, --compress       same as the compress command\n"
      "  -9                   maximum effort preset (no CM backend)\n"
      "      --max-ratio      allow the CM backend (slowest, smallest)\n"
      "      --no-precomp     disable semantic precompression\n"
      "      --no-lzma2       disable the LZMA2 fallback (breaks Tier-1)\n"
      "      --no-parse       disable the optimal parse (RAW blocks only)\n"
      "      --no-adaptive    disable adaptive tables (static rANS only)\n"
      "      --no-range       disable the range-coded model\n"
      "      --no-bcj         disable the x86 BCJ transform\n"
      "      --no-coarse      disable the coarse match index\n"
      "  -b, --block-size N   block size in KiB (default 256)\n"
      "  -o, --output FILE    write to FILE instead of stdout\n"
      "  -f, --force          overwrite an existing output file\n"
      "  -v, --verbose        print per-run statistics to stderr\n"
      "  -h, --help           print this text and exit\n"
      "      --version        print version and exit\n"
      "\n"
      "Omit input/output to filter stdin to stdout.\n");
}

// Reads a whole stream. Returns false on a read error (not on EOF).
bool read_all(FILE* f, std::vector<uint8_t>& out) {
  uint8_t chunk[1 << 16];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    out.insert(out.end(), chunk, chunk + n);
  }
  return std::ferror(f) == 0;
}

bool write_all(FILE* f, const std::vector<uint8_t>& data) {
  if (!data.empty() &&
      std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
    return false;
  }
  return std::fflush(f) == 0;
}

int fail(const char* what, chce::Err e) {
  std::fprintf(stderr, "ziptastic: %s: %s\n", what, chce::err_message(e));
  return 1;
}

struct Options {
  enum class Command { Compress, Decompress, Bench, Version, Help } cmd =
      Command::Compress;
  chce::CompressOptions comp;
  std::string input;
  std::string output;
  bool force = false;
  bool verbose = false;
};

// Parses argv. Returns -1 to continue, or an exit code to return immediately.
int parse_args(int argc, char** argv, Options& o) {
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "ziptastic: %s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };

    if (a == "-h" || a == "--help") {
      o.cmd = Options::Command::Help;
      return 0;
    } else if (a == "--version" || a == "version") {
      o.cmd = Options::Command::Version;
      return 0;
    } else if (a == "help") {
      o.cmd = Options::Command::Help;
      return 0;
    } else if (a == "compress" || a == "c" || a == "-z" || a == "--compress") {
      o.cmd = Options::Command::Compress;
    } else if (a == "decompress" || a == "d" || a == "-d" || a == "--decompress") {
      o.cmd = Options::Command::Decompress;
    } else if (a == "bench") {
      o.cmd = Options::Command::Bench;
    } else if (a == "-9") {
      o.comp.block_size = chce::kDefaultBlockSize;
      o.comp.precomp = true;
      o.comp.coarse = true;
    } else if (a == "--max-ratio") {
      o.comp.max_ratio = true;
      o.comp.precomp = true;
      o.comp.coarse = true;
    } else if (a == "--no-precomp") {
      o.comp.precomp = false;
    } else if (a == "--no-lzma2") {
      o.comp.lzma2_fallback = false;
    } else if (a == "--no-parse") {
      o.comp.parse = false;
    } else if (a == "--no-adaptive") {
      o.comp.adaptive = false;
    } else if (a == "--no-range") {
      o.comp.range = false;
    } else if (a == "--no-bcj") {
      o.comp.bcj = false;
    } else if (a == "--no-coarse") {
      o.comp.coarse = false;
    } else if (a == "-b" || a == "--block-size") {
      const char* v = need_value("--block-size");
      if (!v) return 2;
      char* end = nullptr;
      const unsigned long kib = std::strtoul(v, &end, 10);
      if (end == v || *end != '\0' || kib == 0) {
        std::fprintf(stderr, "ziptastic: invalid block size '%s'\n", v);
        return 2;
      }
      o.comp.block_size = static_cast<size_t>(kib) * 1024;
    } else if (a == "-o" || a == "--output") {
      const char* v = need_value("--output");
      if (!v) return 2;
      o.output = v;
    } else if (a == "-f" || a == "--force") {
      o.force = true;
    } else if (a == "-v" || a == "--verbose") {
      o.verbose = true;
    } else if (!a.empty() && a[0] == '-' && a != "-") {
      std::fprintf(stderr, "ziptastic: unknown option '%s' (try --help)\n", a.c_str());
      return 2;
    } else {
      positional.push_back(a);
    }
  }

  if (positional.size() > 2) {
    std::fprintf(stderr, "ziptastic: too many arguments (try --help)\n");
    return 2;
  }
  if (!positional.empty() && positional[0] != "-") o.input = positional[0];
  if (positional.size() == 2 && positional[1] != "-") o.output = positional[1];
  return -1;
}

// Opens the output sink, refusing to clobber unless -f was given.
FILE* open_output(const Options& o, bool& ok) {
  ok = true;
  if (o.output.empty()) return stdout;
  std::error_code ec;
  if (!o.force && fs::exists(o.output, ec)) {
    std::fprintf(stderr, "ziptastic: %s exists (use -f to overwrite)\n",
                 o.output.c_str());
    ok = false;
    return nullptr;
  }
  FILE* f = std::fopen(o.output.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "ziptastic: cannot write %s\n", o.output.c_str());
    ok = false;
  }
  return f;
}

int run_compress(const Options& o) {
  std::vector<uint8_t> input;
  FILE* in = o.input.empty() ? stdin : std::fopen(o.input.c_str(), "rb");
  if (!in) {
    std::fprintf(stderr, "ziptastic: cannot read %s\n", o.input.c_str());
    return 1;
  }
  const bool read_ok = read_all(in, input);
  if (in != stdin) std::fclose(in);
  if (!read_ok) {
    std::fprintf(stderr, "ziptastic: read error on %s\n",
                 o.input.empty() ? "<stdin>" : o.input.c_str());
    return 1;
  }

  std::vector<uint8_t> out;
  chce::CompressStats stats;
  const chce::Err e = chce::compress(input, out, o.comp, &stats);
  if (e != chce::Err::Ok) return fail("compression failed", e);

  bool ok = true;
  FILE* fo = open_output(o, ok);
  if (!ok) return 1;
  const bool wrote = write_all(fo, out);
  if (fo != stdout) std::fclose(fo);
  if (!wrote) {
    std::fprintf(stderr, "ziptastic: write error\n");
    return 1;
  }

  if (o.verbose) {
    std::fprintf(stderr,
                 "ziptastic: %zu -> %zu bytes (%.2f%%), %llu blocks (%llu raw, "
                 "%llu lzma2, %llu parsed)%s\n",
                 input.size(), out.size(),
                 input.empty() ? 100.0
                               : 100.0 * static_cast<double>(out.size()) /
                                     static_cast<double>(input.size()),
                 static_cast<unsigned long long>(stats.blocks),
                 static_cast<unsigned long long>(stats.raw_blocks),
                 static_cast<unsigned long long>(stats.lzma2_blocks),
                 static_cast<unsigned long long>(stats.parsed_blocks),
                 stats.lzma2_stream ? " [lzma2 fallback]" : "");
  }
  return 0;
}

int run_decompress(const Options& o) {
  std::vector<uint8_t> input;
  FILE* in = o.input.empty() ? stdin : std::fopen(o.input.c_str(), "rb");
  if (!in) {
    std::fprintf(stderr, "ziptastic: cannot read %s\n", o.input.c_str());
    return 1;
  }
  const bool read_ok = read_all(in, input);
  if (in != stdin) std::fclose(in);
  if (!read_ok) {
    std::fprintf(stderr, "ziptastic: read error on %s\n",
                 o.input.empty() ? "<stdin>" : o.input.c_str());
    return 1;
  }

  std::vector<uint8_t> out;
  chce::DecompressReport rep;
  const chce::Err e = chce::decompress(input, out, &rep);
  if (e != chce::Err::Ok) return fail("decompression failed", e);

  bool ok = true;
  FILE* fo = open_output(o, ok);
  if (!ok) return 1;
  const bool wrote = write_all(fo, out);
  if (fo != stdout) std::fclose(fo);
  if (!wrote) {
    std::fprintf(stderr, "ziptastic: write error\n");
    return 1;
  }

  if (o.verbose) {
    std::fprintf(stderr, "ziptastic: %zu -> %zu bytes, %llu blocks\n", input.size(),
                 out.size(), static_cast<unsigned long long>(rep.blocks));
  }
  return 0;
}

int run_bench(const Options& o) {
  std::vector<std::string> files;
  if (!o.input.empty()) {
    std::error_code ec;
    if (fs::is_directory(o.input, ec)) {
      for (const auto& de : fs::directory_iterator(o.input, ec)) {
        if (de.is_regular_file()) files.push_back(de.path().string());
      }
      std::sort(files.begin(), files.end());
    } else {
      files.push_back(o.input);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "ziptastic: bench needs an input file or directory\n");
    return 2;
  }

  uint64_t total_in = 0, total_out = 0;
  std::printf("%-28s %12s %12s %8s %10s\n", "file", "raw", "chce", "ratio",
              "MB/s");
  for (const auto& path : files) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "ziptastic: cannot read %s\n", path.c_str());
      continue;
    }
    std::vector<uint8_t> data;
    read_all(f, data);
    std::fclose(f);

    std::vector<uint8_t> out;
    const auto t0 = std::chrono::steady_clock::now();
    const chce::Err e = chce::compress(data, out, o.comp, nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    if (e != chce::Err::Ok) {
      std::fprintf(stderr, "ziptastic: %s: %s\n", path.c_str(),
                   chce::err_message(e));
      continue;
    }

    const double secs =
        std::chrono::duration<double>(t1 - t0).count();
    const double mbs = secs > 0 ? static_cast<double>(data.size()) / secs / 1e6 : 0.0;
    const double ratio =
        data.empty() ? 0.0
                     : 100.0 * static_cast<double>(out.size()) /
                           static_cast<double>(data.size());

    std::string name = fs::path(path).filename().string();
    if (name.size() > 28) name = name.substr(0, 28);
    std::printf("%-28s %12zu %12zu %7.2f%% %10.1f\n", name.c_str(), data.size(),
                out.size(), ratio, mbs);
    total_in += data.size();
    total_out += out.size();
  }

  const double total_ratio =
      total_in ? 100.0 * static_cast<double>(total_out) / static_cast<double>(total_in)
               : 0.0;
  std::printf("%-28s %12llu %12llu %7.2f%%\n", "TOTAL",
              static_cast<unsigned long long>(total_in),
              static_cast<unsigned long long>(total_out), total_ratio);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  const int early = parse_args(argc, argv, o);
  if (early >= 0) {
    if (o.cmd == Options::Command::Help) {
      print_usage(stdout);
      return 0;
    }
    if (o.cmd == Options::Command::Version) {
      std::printf("%s\n", kVersionString);
      return 0;
    }
    return early;
  }

  switch (o.cmd) {
    case Options::Command::Compress: return run_compress(o);
    case Options::Command::Decompress: return run_decompress(o);
    case Options::Command::Bench: return run_bench(o);
    case Options::Command::Version: std::printf("%s\n", kVersionString); return 0;
    case Options::Command::Help: print_usage(stdout); return 0;
  }
  return 0;
}
