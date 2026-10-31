// cm_zpaq.cpp — thin wrapper around libzpaq.
#include "cm_zpaq.hpp"

#include <cstring>

#include <libzpaq.h>

#include <stdexcept>

// libzpaq declares error() but expects the host program to define it; without
// this the library links but has no way to report a failure. Throwing is right
// here because every call below is wrapped in try/catch and turned into a
// "declined" or "corrupt" answer.
namespace libzpaq {
void error(const char* msg) { throw std::runtime_error(msg); }
}  // namespace libzpaq

namespace chce {

bool zpaq_available() { return true; }

bool zpaq_compress_block(const std::vector<uint8_t>& block, std::vector<uint8_t>& out) {
  if (block.empty()) return false;

  libzpaq::StringBuffer in;
  in.write(reinterpret_cast<const char*>(block.data()),
           static_cast<int>(block.size()));

  libzpaq::StringBuffer packed;
  // Method "5" is the strongest numeric level compressBlock offers; the deeper
  // context-mixing configurations are config strings, which are a tuning matter
  // for Phase 12 rather than something to guess at here.
  try {
    libzpaq::compressBlock(&in, &packed, "5", nullptr, nullptr, false);
  } catch (...) {
    return false;
  }
  if (packed.size() == 0) return false;
  if (packed.size() >= block.size()) return false;  // no gain

  out.assign(reinterpret_cast<const uint8_t*>(packed.c_str()),
             reinterpret_cast<const uint8_t*>(packed.c_str()) + packed.size());
  return true;
}

bool zpaq_decompress_block(const std::vector<uint8_t>& body, uint32_t out_size,
                           std::vector<uint8_t>& out) {
  if (body.empty()) return false;

  libzpaq::StringBuffer in;
  in.write(reinterpret_cast<const char*>(body.data()), static_cast<int>(body.size()));

  libzpaq::StringBuffer unpacked;
  try {
    libzpaq::decompress(&in, &unpacked);
  } catch (...) {
    return false;
  }
  if (unpacked.size() != out_size) return false;

  const uint8_t* p = reinterpret_cast<const uint8_t*>(unpacked.c_str());
  out.insert(out.end(), p, p + unpacked.size());
  return true;
}

}  // namespace chce
