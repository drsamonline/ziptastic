// xz_probe.cpp — can the .xz transform be built on a header read?
//
// The question Phase 4 left open (FINDINGS.md §3h): a stream's producing settings must be recovered
// before the stream can be rebuilt byte-exactly, and a search over settings costs 2.5-4x the whole
// compression. For .xz the dictionary size is in the block header, so the question is whether that is
// *enough* — whether, given only the dict size read from the stream, re-encoding reproduces it.
//
// This probe uses the vendored liblzma, not the system one, because the answer depends on the encoder
// that will actually ship.
//
//   xz_probe <file.xz>
//
// It prints, for each preset the tree's liblzma knows: the decoded size, the dict size read from the
// block header, and whether re-encoding at that preset with that dict size is byte-identical.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <lzma.h>

namespace {

bool read_all(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

// The whole file as one stream, decoded. Bounded so a hostile stream cannot exhaust memory.
bool decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return false;
  strm.next_in = in.data();
  strm.avail_in = in.size();
  uint8_t buf[1 << 16];
  lzma_ret ret = LZMA_OK;
  while (ret == LZMA_OK) {
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    ret = lzma_code(&strm, LZMA_FINISH);
    const size_t got = sizeof(buf) - strm.avail_out;
    out.insert(out.end(), buf, buf + got);
    if (out.size() > (256u << 20)) {  // a fixture, not a product
      lzma_end(&strm);
      return false;
    }
  }
  lzma_end(&strm);
  return ret == LZMA_STREAM_END;
}

// Reads the LZMA2 dictionary size out of the first block's filter list. This is the whole premise:
// if it can be read here, the producing settings need not be searched for.
bool block_dict_size(const std::vector<uint8_t>& in, uint32_t& dict_size) {
  lzma_stream_flags flags;
  if (lzma_stream_header_decode(&flags, in.data()) != LZMA_OK) return false;
  // 12-byte stream header, then the block header (whose size is in its first byte).
  const size_t block_at = 12;
  if (in.size() <= block_at) return false;
  const uint32_t header_size = (static_cast<uint32_t>(in[block_at]) + 1) * 4;
  if (in.size() < block_at + header_size) return false;

  lzma_filter filters[LZMA_FILTERS_MAX + 1];
  lzma_block block;
  std::memset(&block, 0, sizeof(block));
  block.version = 0;
  block.check = flags.check;
  block.filters = filters;
  // Required: lzma_block_header_decode reads this, it does not compute it.
  block.header_size = lzma_block_header_size_decode(in[block_at]);
  if (lzma_block_header_decode(&block, nullptr, in.data() + block_at) != LZMA_OK) return false;
  for (int i = 0; filters[i].id != LZMA_VLI_UNKNOWN; ++i) {
    if (filters[i].id != LZMA_FILTER_LZMA2) continue;
    const auto* opt = static_cast<const lzma_options_lzma*>(filters[i].options);
    dict_size = opt->dict_size;
    return true;
  }
  return false;
}

bool encode(const std::vector<uint8_t>& raw, uint32_t preset, uint32_t dict_size,
            std::vector<uint8_t>& out) {
  lzma_options_lzma opt;
  if (lzma_lzma_preset(&opt, preset)) return false;
  opt.dict_size = dict_size;  // the one parameter the stream told us

  lzma_filter filters[2];
  filters[0].id = LZMA_FILTER_LZMA2;
  filters[0].options = &opt;
  filters[1].id = LZMA_VLI_UNKNOWN;
  filters[1].options = nullptr;

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK) return false;
  strm.next_in = raw.data();
  strm.avail_in = raw.size();
  uint8_t buf[1 << 16];
  lzma_ret ret = LZMA_OK;
  while (ret == LZMA_OK) {
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    ret = lzma_code(&strm, LZMA_FINISH);
    const size_t got = sizeof(buf) - strm.avail_out;
    out.insert(out.end(), buf, buf + got);
  }
  lzma_end(&strm);
  return ret == LZMA_STREAM_END;
}

const char* preset_name(uint32_t p) {
  static char names[4][16];
  static int next = 0;
  char* slot = names[next++ & 3];
  if ((p & LZMA_PRESET_EXTREME) != 0) {
    std::snprintf(slot, 16, "%u|EXTREME", p & 0x1Fu);
  } else {
    std::snprintf(slot, 16, "%u", p);
  }
  return slot;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: xz_probe <file.xz>\n");
    return 2;
  }
  std::vector<uint8_t> stream;
  if (!read_all(argv[1], stream)) {
    std::fprintf(stderr, "xz_probe: cannot read %s\n", argv[1]);
    return 1;
  }

  std::vector<uint8_t> raw;
  if (!decode(stream, raw)) {
    std::fprintf(stderr, "xz_probe: %s does not decode\n", argv[1]);
    return 1;
  }

  uint32_t dict_size = 0;
  const bool have_dict = block_dict_size(stream, dict_size);
  std::printf("%s  (vendored liblzma %s)\n", argv[1], lzma_version_string());
  std::printf("  stream %zu bytes, decoded %zu bytes, dict from header %s%u\n", stream.size(),
              raw.size(), have_dict ? "" : "NOT READ ", dict_size);

  const uint32_t presets[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9 | LZMA_PRESET_EXTREME};
  int matches = 0;
  for (uint32_t p : presets) {
    std::vector<uint8_t> again;
    if (!encode(raw, p, have_dict ? dict_size : 0, again)) continue;
    const bool same = again == stream;
    if (same) ++matches;
    std::printf("  preset %-9s -> %zu bytes%s\n", preset_name(p), again.size(),
                same ? "   IDENTICAL" : "");
  }
  std::printf("  %s\n", matches > 0 ? "RECOVERABLE from the header alone"
                                    : "not reproducible: a diff would be needed");
  return matches > 0 ? 0 : 1;
}
