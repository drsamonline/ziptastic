// bwt_bsc.cpp — thin wrapper around libbsc.
#include "bwt_bsc.hpp"

#include <cstring>
#include <mutex>

#include <libbsc/libbsc.h>
#include <libbsc/platform/platform.h>

namespace chce {
namespace {

// libbsc needs a one-time init. Guarded so it happens exactly once even if
// compression is ever called from several threads.
void ensure_init() {
  static std::once_flag once;
  std::call_once(once, []() {
    bsc_init(LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING);
  });
}

}  // namespace

bool bsc_available() { return true; }

bool bsc_compress_block(std::span<const uint8_t> block, std::vector<uint8_t>& out) {
  if (block.empty()) return false;
  ensure_init();

  out.resize(block.size() + LIBBSC_HEADER_SIZE);
  // Parameters: no LZP (we have our own matcher), BWT sorter, adaptive QLFC.
  // lzpHashSize / lzpMinLen are 0: we have our own match finder and do not want
  // libbsc running a second one inside the block.
  const int n = bsc_compress(block.data(), out.data(), static_cast<int>(block.size()),
                             0, 0, LIBBSC_BLOCKSORTER_BWT,
                             LIBBSC_CODER_QLFC_ADAPTIVE, LIBBSC_FEATURE_FASTMODE);
  if (n <= 0) {
    out.clear();
    return false;  // libbsc says "not compressible"
  }
  if (static_cast<size_t>(n) >= block.size()) {
    out.clear();
    return false;  // no gain: the selector would not pick it anyway
  }
  out.resize(static_cast<size_t>(n));
  return true;
}

Err bsc_decompress_block(std::span<const uint8_t> body, uint32_t out_size,
                         std::vector<uint8_t>& out) {
  if (body.size() < static_cast<size_t>(LIBBSC_HEADER_SIZE)) return Err::Truncated;
  ensure_init();

  int block_size = 0;
  int data_size = 0;
  if (bsc_block_info(body.data(), static_cast<int>(body.size()), &block_size, &data_size,
                     LIBBSC_FEATURE_FASTMODE) != LIBBSC_NO_ERROR) {
    return Err::Corrupt;
  }
  if (data_size < 0 || static_cast<uint32_t>(data_size) != out_size) {
    return Err::SizeMismatch;
  }

  const size_t start = out.size();
  out.resize(start + out_size);
  const int rc = bsc_decompress(body.data(), static_cast<int>(body.size()),
                                out.data() + start, static_cast<int>(out_size),
                                LIBBSC_FEATURE_FASTMODE);
  if (rc != LIBBSC_NO_ERROR) {
    out.resize(start);
    return Err::Corrupt;
  }
  return Err::Ok;
}

}  // namespace chce
