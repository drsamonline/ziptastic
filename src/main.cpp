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
#include "chimera/block_index.hpp"
#include "chimera/format.hpp"

namespace fs = std::filesystem;

namespace {

// Built from the header rather than a literal: this string said "v1" through four
// format changes, which is the kind of thing nobody notices until a user reports the
// wrong version back.
// The release version, in one place. `--version` prints it beside the format version, because
// the two change for different reasons and a bug report needs both.
// The release version comes from CMake (project(... VERSION ...)) so there is exactly one place
// it is written down. The fallback exists so a build that bypasses CMake still compiles.
#ifndef CHCE_VERSION
#define CHCE_VERSION "0.0.0-unknown"
#endif
constexpr const char* kReleaseVersion = CHCE_VERSION;

// The archive extension. Detection is by the CHCE magic, never by this — a .dr renamed to .bin
// must still decode, and a .dr that is not a CHCE stream must be refused. The extension is a
// convention for humans and file managers.
constexpr const char* kArchiveExtension = ".dr";

const char* version_string() {
  static const std::string s = std::string("ziptastic ") + kReleaseVersion +
                               " (CHCE format v" +
                               std::to_string(static_cast<int>(chce::kFormatVersion)) + ")";
  return s.c_str();
}

void print_usage(FILE* f) {
  std::fprintf(f,
      "ziptastic — CHCE (Chimera Hybrid Compression Engine)\n"
      "\n"
      "usage: ziptastic [command] [options] [input] [output]\n"
      "\n"
      "commands:\n"
      "  compress, c        compress input to output (default)\n"
      "  decompress, d      decompress input to output\n"
      "  info, i            print the header, the block table and its offsets\n"
      "  bench              compress and report ratio + speed, no output file\n"
      "  version            print version and exit\n"
      "  help               print this text and exit\n"
      "\n"
      "options:\n"
      "  -d, --decompress     same as the decompress command\n"
      "  -z, --compress       same as the compress command\n"
      "  -9                   maximum effort preset (no CM backend)\n"
      "      --max-ratio      allow the CM backend (slowest, smallest)\n"
      "      --no-cm-stream   with --max-ratio, skip the whole-stream CM pass\n"
      "      --no-precomp     disable semantic precompression\n"
      "      --no-numeric     disable the integer CSV/TSV column transform\n"
      "      --no-base64      disable the base64 decode transform\n"
      "      --no-wav         disable the WAV per-channel delta transform\n"
      "      --no-sqlite      disable the SQLite page-grouping transform\n"
      "      --no-ecc         disable CD sector parity regeneration\n"
      "      --no-bmp         disable the BMP colour-plane transform\n"
      "      --split-size N   write output as N-byte parts (suffixes K/M/G)\n"
      "      --no-bwt         disable the BWT backend candidate\n"
      "      --no-huffman     disable the Huffman backend candidates\n"
      "      --no-huff-static  disable the static Huffman candidate only\n"
      "      --huff-adapt     re-enable the adaptive Huffman candidate (off by default)\n"
      "  -T, --threads N      threads for --parallel (default: all cores)\n"
      "      --parallel       self-contained blocks, compressed on N threads\n"
      "      --no-lzma2       disable the LZMA2 fallback (breaks Tier-1)\n"
      "      --no-parse       disable the optimal parse (RAW blocks only)\n"
      "      --no-adaptive    disable adaptive tables (static rANS only)\n"
      "      --no-range       disable the range-coded model\n"
      "      --no-bcj         disable the x86 BCJ transform\n"
      "      --no-coarse      disable the coarse match index\n"
      "  -b, --block-size N   block size in KiB (default 4096)\n"
      "      --block N        with d: decode block N alone (needs --parallel blocks)\n"
      "      --from-block N   with d: decode blocks N to the end (resumable)\n"
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

// ---------------------------------------------------------------------------
// Split output
// ---------------------------------------------------------------------------
//
// `--split-size N` writes out.001, out.002, ... Each part ends with a 16-byte
// footer: u32 index, u32 count, u32 total stream length, "ZS01". The footer is what
// makes a part distinguishable from a truncated stream and lets the decoder check it
// has every piece before it starts.
//
// The parts are pieces of one already-compressed stream, so they must all be present
// to decode: a part on its own is not a stream. That is the same trade split(1)
// makes, and it costs nothing in ratio -- unlike splitting the input, which measured
// at about 2x (docs/SPLIT_ARCHIVES.md).

constexpr size_t kPartFooter = 16;

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint32_t get_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string part_name(const std::string& base, uint32_t index) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), ".%03u", index + 1);
  return base + buf;
}

bool write_parts(const std::string& base, const std::vector<uint8_t>& data, size_t part_size) {
  const size_t count = (data.size() + part_size - 1) / part_size;
  if (count == 0 || count > 999) {
    std::fprintf(stderr, "ziptastic: cannot split into %zu parts (1-999)\n", count);
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const size_t from = i * part_size;
    const size_t len = std::min(part_size, data.size() - from);
    std::vector<uint8_t> part(data.begin() + static_cast<long>(from),
                               data.begin() + static_cast<long>(from + len));
    put_u32(part, static_cast<uint32_t>(i));
    put_u32(part, static_cast<uint32_t>(count));
    put_u32(part, static_cast<uint32_t>(data.size()));
    part.push_back('Z');
    part.push_back('S');
    part.push_back('0');
    part.push_back('1');
    const std::string name = part_name(base, static_cast<uint32_t>(i));
    FILE* f = std::fopen(name.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "ziptastic: cannot write %s\n", name.c_str());
      return false;
    }
    const bool ok = write_all(f, part);
    std::fclose(f);
    if (!ok) return false;
  }
  std::fprintf(stderr, "ziptastic: wrote %zu parts\n", count);
  return true;
}

// True when `data` is one part of a split set, in which case `out` is the joined
// stream. Reads the siblings from disk: a part carries only its own slice.
bool join_parts(const std::string& path, const std::vector<uint8_t>& first,
                std::vector<uint8_t>& out) {
  if (first.size() < kPartFooter) return false;
  const uint8_t* tail = first.data() + first.size() - kPartFooter;
  if (tail[12] != 'Z' || tail[13] != 'S' || tail[14] != '0' || tail[15] != '1') return false;

  const uint32_t index = get_u32(tail);
  const uint32_t count = get_u32(tail + 4);
  const uint32_t total = get_u32(tail + 8);
  if (count == 0 || count > 999 || index >= count) return false;
  if (path.size() < 4) return false;
  const std::string base = path.substr(0, path.size() - 4);  // strip ".001"

  out.clear();
  out.reserve(total);
  for (uint32_t i = 0; i < count; ++i) {
    std::vector<uint8_t> buf;
    if (i == index) {
      buf = first;
    } else {
      const std::string name = part_name(base, i);
      FILE* f = std::fopen(name.c_str(), "rb");
      if (!f) {
        std::fprintf(stderr, "ziptastic: part %u of %u is missing (%s)\n", i + 1, count,
                     name.c_str());
        return false;
      }
      const bool ok = read_all(f, buf);
      std::fclose(f);
      if (!ok) return false;
    }
    if (buf.size() < kPartFooter) return false;
    const uint8_t* t = buf.data() + buf.size() - kPartFooter;
    if (t[12] != 'Z' || t[13] != 'S' || t[14] != '0' || t[15] != '1') return false;
    if (get_u32(t) != i || get_u32(t + 4) != count) {
      std::fprintf(stderr, "ziptastic: part %u is out of place or from another set\n", i + 1);
      return false;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(buf.size() - kPartFooter));
  }
  if (out.size() != total) {
    std::fprintf(stderr, "ziptastic: joined %zu bytes, expected %u\n", out.size(), total);
    return false;
  }
  std::fprintf(stderr, "ziptastic: joined %u parts\n", count);
  return true;
}

int fail(const char* what, chce::Err e) {
  std::fprintf(stderr, "ziptastic: %s: %s\n", what, chce::err_message(e));
  return 1;
}

struct Options {
  // --split-size: bytes per output part. 0 keeps one file.
  size_t split_size = 0;
  enum class Command { Compress, Decompress, Bench, Info, Version, Help } cmd =
      Command::Compress;
  chce::CompressOptions comp;
  std::string input;
  std::string output;
  bool force = false;
  bool verbose = false;
  // --block N / --from-block N. -1 means "the whole stream". Two flags rather than one
  // range, because they are two different requests: one block, or everything from a
  // block onwards (which is what a crashed decode resumes with).
  long long block = -1;
  long long from_block = -1;
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
    } else if (a == "info" || a == "i") {
      o.cmd = Options::Command::Info;
    } else if (a == "--block") {
      const char* v = need_value("--block");
      if (!v) return 2;
      o.block = std::strtoll(v, nullptr, 10);
      if (o.block < 0) {
        std::fprintf(stderr, "ziptastic: --block needs a block index\n");
        return 2;
      }
    } else if (a == "--from-block") {
      const char* v = need_value("--from-block");
      if (!v) return 2;
      o.from_block = std::strtoll(v, nullptr, 10);
      if (o.from_block < 0) {
        std::fprintf(stderr, "ziptastic: --from-block needs a block index\n");
        return 2;
      }
    } else if (a == "-9") {
      o.comp.block_size = chce::kDefaultBlockSize;
      o.comp.precomp = true;
      o.comp.coarse = true;
    } else if (a == "--max-ratio") {
      o.comp.max_ratio = true;
      o.comp.precomp = true;
      o.comp.coarse = true;
    } else if (a == "--parallel") {
      o.comp.parallel = true;
    } else if (a == "-T" || a == "--threads") {
      if (i + 1 < argc) o.comp.threads = static_cast<unsigned>(std::atoi(argv[++i]));
    } else if (a == "--split-size") {
      if (i + 1 < argc) {
        std::string v = argv[++i];
        size_t mul = 1;
        if (!v.empty() && (v.back() == 'K' || v.back() == 'k')) { mul = 1024; v.pop_back(); }
        else if (!v.empty() && (v.back() == 'M' || v.back() == 'm')) { mul = 1024u * 1024u; v.pop_back(); }
        else if (!v.empty() && (v.back() == 'G' || v.back() == 'g')) { mul = 1024u * 1024u * 1024u; v.pop_back(); }
        o.split_size = static_cast<size_t>(std::strtoull(v.c_str(), nullptr, 10)) * mul;
        if (o.split_size < 4096) {
          std::fprintf(stderr, "ziptastic: --split-size must be at least 4K\n");
          return 2;
        }
      }
    } else if (a == "--no-bwt") {
      o.comp.bwt = false;
    } else if (a == "--no-huffman") {
      o.comp.huffman = false;
    } else if (a == "--no-huff-static") {
      o.comp.huff_static = false;
    } else if (a == "--huff-adapt") {
      o.comp.huff_adaptive = true;
    } else if (a == "--no-bmp") {
      o.comp.bmp = false;
    } else if (a == "--no-ecc") {
      o.comp.ecc = false;
    } else if (a == "--no-sqlite") {
      o.comp.sqlite = false;
    } else if (a == "--no-wav") {
      o.comp.wav = false;
    } else if (a == "--no-base64") {
      o.comp.base64 = false;
    } else if (a == "--no-numeric") {
      o.comp.numeric = false;
    } else if (a == "--no-cm-stream") {
      o.comp.cm_stream = false;
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
  // `ziptastic c file` with no output path writes file.dr. It is the first thing anyone types,
  // and requiring -o for the common case is friction with no purpose.
  if (o.output.empty() && o.cmd == Options::Command::Compress && !o.input.empty() &&
      o.input != "-") {
    o.output = o.input + kArchiveExtension;
  }
  // Decoding an `x.dr` writes `x`; anything else writes `x.out`, so a stream that was renamed
  // never overwrites the file it came from.
  if (o.output.empty() && o.cmd == Options::Command::Decompress && !o.input.empty() &&
      o.input != "-") {
    const std::string ext = kArchiveExtension;
    const std::string in = o.input;
    if (in.size() > ext.size() &&
        in.compare(in.size() - ext.size(), ext.size(), ext) == 0) {
      o.output = in.substr(0, in.size() - ext.size());
    } else {
      o.output = in + ".out";
    }
  }
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

// Defined below, beside the other decode commands: `d --block` needs the offset table.
int run_partial_decode(const Options& o, const std::vector<uint8_t>& input);

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

  if (o.split_size > 0) {
    if (o.output.empty()) {
      std::fprintf(stderr, "ziptastic: --split-size needs an output file name\n");
      return 1;
    }
    if (!write_parts(o.output, out, o.split_size)) return 1;
    return 0;
  }

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
  if (o.verbose && stats.huff_trials > 0) {
    std::fprintf(stderr, "  huffman: %llu offered, %llu skipped by the sample trial\n",
                 static_cast<unsigned long long>(stats.huff_trials),
                 static_cast<unsigned long long>(stats.huff_skipped));
  }
  if (o.verbose && !stats.lzma2_stream &&
      (stats.bwt_blocks || stats.huff_blocks)) {
    std::fprintf(stderr, "  chose: %llu BWT, %llu huffman\n",
                 static_cast<unsigned long long>(stats.bwt_blocks),
                 static_cast<unsigned long long>(stats.huff_blocks));
  }
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

  // A part of a split set is not a stream. Join the siblings before decoding, so
  // `ziptastic d out.001` works the way the name suggests it should.
  if (!o.input.empty()) {
    std::vector<uint8_t> joined;
    if (join_parts(o.input, input, joined)) input = std::move(joined);
  }

  // --block / --from-block: answer from the offset table instead of decoding the stream.
  if (o.block >= 0 || o.from_block >= 0) {
    if (o.block >= 0 && o.from_block >= 0) {
      std::fprintf(stderr, "ziptastic: --block and --from-block are alternatives\n");
      return 2;
    }
    return run_partial_decode(o, input);
  }

  std::vector<uint8_t> out;
  chce::DecompressReport rep;
  const chce::Err e = chce::decompress(input, out, &rep);
  if (e != chce::Err::Ok) {
    // A block-level failure names the block: that is what the digest is for.
    if (e == chce::Err::BlockChecksumMismatch && rep.bad_block != chce::kNoBlock) {
      std::fprintf(stderr,
                   "ziptastic: decompression failed: %s (block %llu of %llu)\n",
                   chce::err_message(e),
                   static_cast<unsigned long long>(rep.bad_block),
                   static_cast<unsigned long long>(rep.blocks ? rep.blocks : 0));
      return 1;
    }
    return fail("decompression failed", e);
  }

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

// Reads a whole input (file or stdin) for the commands that need the bytes in hand.
bool read_input(const Options& o, std::vector<uint8_t>& data) {
  FILE* in = o.input.empty() ? stdin : std::fopen(o.input.c_str(), "rb");
  if (!in) {
    std::fprintf(stderr, "ziptastic: cannot read %s\n", o.input.c_str());
    return false;
  }
  const bool ok = read_all(in, data);
  if (in != stdin) std::fclose(in);
  if (!ok) {
    std::fprintf(stderr, "ziptastic: read error on %s\n",
                 o.input.empty() ? "<stdin>" : o.input.c_str());
  }
  return ok;
}

const char* kFlagNames[8] = {"coarse", "max-ratio", "precomp", "bcj",
                             "parallel", "dedup", "jpeg", "gif"};

std::string describe_flags(uint8_t f) {
  std::string s;
  for (int i = 0; i < 8; ++i) {
    if ((f & (1u << i)) == 0) continue;
    if (!s.empty()) s += ", ";
    s += kFlagNames[i];
  }
  return s.empty() ? "none" : s;
}

// `ziptastic info` — the header, and the block table that v8 added.
//
// This is the command the offset table needed to justify itself. An index nothing reads is
// an index that rots; every field printed here is read through `read_index`, which
// validates the table against the manifest before any of it is shown.
int run_info(const Options& o) {
  std::vector<uint8_t> input;
  if (!read_input(o, input)) return 1;

  chce::StreamIndex idx;
  const chce::Err e = chce::read_index(input, idx);
  if (e != chce::Err::Ok) return fail("cannot read the stream index", e);

  const chce::FileHeader& h = idx.header;
  std::printf("%-14s %s\n", "file", o.input.empty() ? "<stdin>" : o.input.c_str());
  std::printf("%-14s %s\n", "format", version_string());
  std::printf("%-14s %s (0x%02x)\n", "flags", describe_flags(h.flags).c_str(), h.flags);
  std::printf("%-14s 0x%02x 0x%02x 0x%02x%s\n", "layers", h.layers, h.layers2, h.layers3,
              (h.layers | h.layers2 | h.layers3) == 0 ? " (no Layer 0 transform)" : "");
  std::printf("%-14s %llu\n", "original size",
              static_cast<unsigned long long>(h.original_size));
  std::printf("%-14s %llu\n", "true size", static_cast<unsigned long long>(h.true_size));
  std::printf("%-14s 0x%08x\n", "crc32", h.checksum);
  std::printf("%-14s %zu\n", "blocks", idx.size());
  if (idx.stream_offset != 0) {
    std::printf("%-14s %llu bytes before the magic\n", "prefix",
                static_cast<unsigned long long>(idx.stream_offset));
  }
  std::printf("%-14s verified against the manifest digest, %llu bytes read of %zu\n",
              "block table", static_cast<unsigned long long>(idx.bytes_read),
              input.size());
  std::printf("%-14s %s\n", "block access",
              chce::blocks_are_independently_decodable(h)
                  ? "each block decodes on its own (--parallel)"
                  : "archive mode: a block may reference an earlier one");

  if (idx.size() == 0) return 0;
  std::printf("\n  %-4s %-12s %-10s %-10s %s\n", "#", "offset", "csize", "usize", "mode");
  for (size_t i = 0; i < idx.size(); ++i) {
    const chce::BlockHeader& b = idx.blocks[i];
    std::printf("  %-4zu 0x%08llx %-10llu %-10llu %s\n", i,
                static_cast<unsigned long long>(idx.block_offsets[i]),
                static_cast<unsigned long long>(b.compressed_size),
                static_cast<unsigned long long>(b.uncompressed_size),
                chce::mode_name(b.mode));
  }
  return 0;
}

// `ziptastic d --block N` / `--from-block N`: decode part of a stream without walking it.
//
// Only a stream whose blocks are self-contained and untransformed can answer this: a
// partial decode cannot run the inverse chain, which needs every byte. Saying that plainly
// beats returning bytes that look right and are not.
int run_partial_decode(const Options& o, const std::vector<uint8_t>& input) {
  chce::StreamIndex idx;
  if (const chce::Err e = chce::read_index(input, idx); e != chce::Err::Ok) {
    return fail("cannot read the stream index", e);
  }
  const uint64_t count = idx.size();
  if (count == 0) {
    std::fprintf(stderr, "ziptastic: the stream has no blocks\n");
    return 1;
  }

  const uint64_t first = static_cast<uint64_t>(o.block >= 0 ? o.block : o.from_block);
  const uint64_t last = o.block >= 0 ? first : count - 1;
  if (first >= count) {
    std::fprintf(stderr, "ziptastic: block %llu does not exist (the stream has %llu)\n",
                 static_cast<unsigned long long>(first),
                 static_cast<unsigned long long>(count));
    return 1;
  }

  if (!chce::blocks_are_independently_decodable(idx.header)) {
    std::fprintf(stderr,
                 "ziptastic: block %llu is not independently decodable: this stream is in "
                 "archive mode, where a block may reference an earlier one.\n"
                 "  re-compress with --parallel to make every block self-contained\n",
                 static_cast<unsigned long long>(first));
    return 1;
  }
  if (!chce::stream_is_untransformed(idx.header)) {
    std::fprintf(stderr,
                 "ziptastic: block %llu cannot be decoded alone: this stream carries a "
                 "Layer 0 or stream-level transform, and its inverse needs the whole "
                 "stream.\n",
                 static_cast<unsigned long long>(first));
    return 1;
  }

  std::vector<uint8_t> out;
  uint64_t bad = chce::kNoBlock;
  const chce::Err e = chce::decode_blocks(input, idx, first, last, out, &bad);
  if (e != chce::Err::Ok) {
    if (e == chce::Err::BlockChecksumMismatch && bad != chce::kNoBlock) {
      std::fprintf(stderr, "ziptastic: block %llu of %llu failed its SHA-256 check\n",
                   static_cast<unsigned long long>(bad),
                   static_cast<unsigned long long>(count));
      return 1;
    }
    return fail("block decode failed", e);
  }

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
                 "ziptastic: blocks %llu..%llu of %llu -> %zu bytes (verified)\n",
                 static_cast<unsigned long long>(first),
                 static_cast<unsigned long long>(last),
                 static_cast<unsigned long long>(count), out.size());
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
      std::printf("%s\n", version_string());
      return 0;
    }
    return early;
  }

  switch (o.cmd) {
    case Options::Command::Compress: return run_compress(o);
    case Options::Command::Decompress: return run_decompress(o);
    case Options::Command::Bench: return run_bench(o);
    case Options::Command::Info: return run_info(o);
    case Options::Command::Version: std::printf("%s\n", version_string()); return 0;
    case Options::Command::Help: print_usage(stdout); return 0;
  }
  return 0;
}
