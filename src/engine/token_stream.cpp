// token_stream.cpp — encode and decode the Phase 3 token body.
#include "token_stream.hpp"

#include <algorithm>

namespace chce {
namespace {

constexpr uint8_t kTagLiterals = 0x00;
constexpr uint8_t kTagMatch = 0x01;

}  // namespace

void encode_tokens(ByteWriter& w, std::span<const uint8_t> data, uint32_t begin,
                   const std::vector<Token>& tokens) {
  uint32_t pos = begin;
  size_t i = 0;
  while (i < tokens.size()) {
    if (tokens[i].is_literal()) {
      // Gather the whole run: a run header amortises to nothing on text and
      // keeps the transitional encoding from being absurd.
      size_t run = 0;
      while (i + run < tokens.size() && tokens[i + run].is_literal()) ++run;
      w.u8(kTagLiterals);
      w.varint(run);
      w.bytes(data.data() + pos, run);
      pos += static_cast<uint32_t>(run);
      i += run;
    } else {
      w.u8(kTagMatch);
      w.varint(tokens[i].len);
      w.varint(tokens[i].dist);
      pos += tokens[i].len;
      ++i;
    }
  }
}

Err decode_tokens(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out) {
  const size_t start = out.size();
  while (out.size() - start < out_size) {
    uint8_t tag = 0;
    if (!r.u8(tag)) return Err::Truncated;

    if (tag == kTagLiterals) {
      uint64_t count = 0;
      if (!r.varint(count)) return Err::BadVarint;
      if (count == 0) return Err::BadMode;
      if (count > out_size - (out.size() - start)) return Err::SizeMismatch;
      std::span<const uint8_t> bytes;
      if (!r.take(static_cast<size_t>(count), bytes)) return Err::Truncated;
      out.insert(out.end(), bytes.begin(), bytes.end());
    } else if (tag == kTagMatch) {
      uint64_t len = 0;
      uint64_t dist = 0;
      if (!r.varint(len)) return Err::BadVarint;
      if (!r.varint(dist)) return Err::BadVarint;
      if (len < kMinMatchLen || len > kMaxMatchLen) return Err::BadMode;
      if (len > out_size - (out.size() - start)) return Err::SizeMismatch;
      // A match may reach back into earlier blocks, but never past the start
      // of the stream.
      if (dist == 0 || dist > out.size()) return Err::BadMode;
      // Byte-at-a-time copy: overlapping matches (dist < len) are legal and
      // are how runs are encoded, so memcpy would be wrong here.
      for (uint64_t k = 0; k < len; ++k) {
        out.push_back(out[out.size() - static_cast<size_t>(dist)]);
      }
    } else {
      return Err::BadMode;
    }
  }
  if (out.size() - start != out_size) return Err::SizeMismatch;
  return Err::Ok;
}

}  // namespace chce
