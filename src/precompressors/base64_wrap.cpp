// base64_wrap.cpp — base64 runs decoded to bytes, with an exact rebuild recipe.
//
// See the header for the measurements that shaped this. The short version: the
// win is the line wrapping, the blob must come first in the payload so downstream
// transforms still recognise what it is, and the single-line structured case is a
// loss — which is why the caller decides with a trial rather than this file
// deciding by rule.
#include "base64_wrap.hpp"

#include <cstring>
#include <string>

#include "numeric_common.hpp"

namespace chce {
namespace {

using namespace numdetail;

constexpr size_t kMinRun = 32;   // shorter runs cost more recipe than they save
constexpr size_t kMinBlob = 64;  // below this there is nothing to win

inline int b64_value(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

const char* alphabet(bool urlsafe) {
  return urlsafe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                 : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

bool decode_core(std::span<const uint8_t> core, std::vector<uint8_t>& out) {
  if (core.empty() || core.size() % 4 == 1) return false;
  uint32_t acc = 0;
  int bits = 0;
  for (uint8_t c : core) {
    const int v = b64_value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

// Encodes without padding; the caller knows the character count it expects and
// compares against it, which is how a non-canonical final group is caught.
void encode_core(std::span<const uint8_t> in, bool urlsafe, std::vector<uint8_t>& out) {
  const char* A = alphabet(urlsafe);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                       (static_cast<uint32_t>(in[i + 1]) << 8) |
                       static_cast<uint32_t>(in[i + 2]);
    out.push_back(static_cast<uint8_t>(A[(v >> 18) & 63]));
    out.push_back(static_cast<uint8_t>(A[(v >> 12) & 63]));
    out.push_back(static_cast<uint8_t>(A[(v >> 6) & 63]));
    out.push_back(static_cast<uint8_t>(A[v & 63]));
  }
  const size_t rem = in.size() - i;
  if (rem == 1) {
    const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    out.push_back(static_cast<uint8_t>(A[(v >> 18) & 63]));
    out.push_back(static_cast<uint8_t>(A[(v >> 12) & 63]));
  } else if (rem == 2) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                       (static_cast<uint32_t>(in[i + 1]) << 8);
    out.push_back(static_cast<uint8_t>(A[(v >> 18) & 63]));
    out.push_back(static_cast<uint8_t>(A[(v >> 12) & 63]));
    out.push_back(static_cast<uint8_t>(A[(v >> 6) & 63]));
  }
}

struct Run {
  size_t at = 0;        // offset in the original input
  size_t core_len = 0;  // characters, excluding '='
  size_t pad = 0;       // '=' characters that followed
  size_t blob_len = 0;  // decoded byte count
};

// The alphabet is a per-stream flag, so it is decided once: a '-' or '_' anywhere
// inside a long alphabet run means URL-safe. A stream that genuinely mixes both
// cannot be described by one flag, and its odd runs fail the re-encode check
// below and stay in the skeleton rather than being guessed at.
bool detect_urlsafe(std::span<const uint8_t> in) {
  for (uint8_t c : in) {
    if (c == '-' || c == '_') return true;
  }
  return false;
}

// Finds maximal alphabet runs and keeps the ones that decode and re-encode to
// exactly the same characters. That one test is what makes MIME wrapping, CRLF,
// stripped padding and URL-safe alphabets work without a case for each: whatever
// cannot be reproduced character for character is left in the skeleton.
bool scan_runs(std::span<const uint8_t> in, std::vector<Run>& runs,
               std::vector<uint8_t>& blob, bool& urlsafe) {
  runs.clear();
  blob.clear();
  urlsafe = detect_urlsafe(in);

  std::vector<uint8_t> decoded;
  std::vector<uint8_t> reencoded;
  size_t i = 0;
  while (i < in.size()) {
    if (b64_value(in[i]) < 0) {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < in.size() && b64_value(in[i]) >= 0) ++i;
    const size_t core_len = i - start;
    size_t pad = 0;
    while (i < in.size() && pad < 2 && in[i] == '=') {
      ++i;
      ++pad;
    }

    // The floor counts the padding too: a 32-character final line whose last
    // group carries '=' has a 31-character core, and rejecting it left the last
    // 23 bytes of the blob behind — which was enough to stop brunsli recognising
    // the JPEG that the whole transform exists to expose.
    if (core_len + pad < kMinRun) continue;
    decoded.clear();
    if (!decode_core(in.subspan(start, core_len), decoded)) continue;
    reencoded.clear();
    encode_core(decoded, urlsafe, reencoded);
    if (reencoded.size() != core_len) continue;
    if (std::memcmp(reencoded.data(), in.data() + start, core_len) != 0) continue;

    Run r;
    r.at = start;
    r.core_len = core_len;
    r.pad = pad;
    r.blob_len = decoded.size();
    blob.insert(blob.end(), decoded.begin(), decoded.end());
    runs.push_back(r);
  }
  return !runs.empty();
}

}  // namespace

bool base64_available() { return true; }

bool base64_likely(std::span<const uint8_t> in) {
  if (in.size() < kMinRun) return false;
  size_t run = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    if (b64_value(in[i]) >= 0) {
      if (++run >= kMinRun) return true;
    } else if (in[i] != '=') {
      run = 0;
    }
  }
  return false;
}

bool base64_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  if (!base64_likely(in)) return false;

  std::vector<Run> runs;
  std::vector<uint8_t> blob;
  bool urlsafe = false;
  if (!scan_runs(in, runs, blob, urlsafe)) return false;
  if (blob.size() < kMinBlob) return false;

  // Skeleton: everything except the accepted runs, padding included.
  std::vector<uint8_t> skeleton;
  skeleton.reserve(in.size() - blob.size());
  {
    size_t pos = 0;
    for (const Run& r : runs) {
      skeleton.insert(skeleton.end(), in.begin() + pos, in.begin() + r.at);
      pos = r.at + r.core_len + r.pad;
    }
    skeleton.insert(skeleton.end(), in.begin() + pos, in.end());
  }

  // Recipe: the alphabet flag, then per run its insertion point as a delta in
  // skeleton coordinates and its character count. Padding rides in the low two
  // bits of the count, because a stripped-padding file has none where the
  // canonical encoding would carry two, so it cannot be inferred.
  std::vector<uint8_t> recipe;
  recipe.push_back(urlsafe ? 1 : 0);
  {
    size_t prev = 0;
    size_t removed = 0;
    for (const Run& r : runs) {
      const size_t at = r.at - removed;
      put_varint(recipe, at - prev);
      put_varint(recipe, r.core_len * 4 + r.pad);
      prev = at;
      removed += r.core_len + r.pad;
    }
  }

  // Layout: blob FIRST. Measured: with a header in front of it, brunsli no longer
  // recognises the JPEG inside and the gain falls from 20.2% to 2.2%; with the
  // blob at offset 0 it fires. The trailer carries the two lengths and the magic,
  // so the payload can still be parsed from the end.
  packed.clear();
  packed.reserve(blob.size() + skeleton.size() + recipe.size() + 12);
  packed.insert(packed.end(), blob.begin(), blob.end());
  packed.insert(packed.end(), skeleton.begin(), skeleton.end());
  packed.insert(packed.end(), recipe.begin(), recipe.end());
  const uint32_t blob_len = static_cast<uint32_t>(blob.size());
  const uint32_t recipe_len = static_cast<uint32_t>(recipe.size());
  for (int i = 0; i < 4; ++i) packed.push_back(static_cast<uint8_t>(recipe_len >> (8 * i)));
  for (int i = 0; i < 4; ++i) packed.push_back(static_cast<uint8_t>(blob_len >> (8 * i)));
  packed.push_back('B');
  packed.push_back('6');
  packed.push_back('4');
  packed.push_back('1');
  return packed.size() < in.size();
}

bool base64_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  // Trailer: u32 recipe_len | u32 blob_len | "B641", 12 bytes at the end.
  if (packed.size() < 12 + 1) return false;
  const size_t t = packed.size() - 12;
  if (packed[t + 8] != 'B' || packed[t + 9] != '6' || packed[t + 10] != '4' ||
      packed[t + 11] != '1') {
    return false;
  }
  uint32_t recipe_len = 0, blob_len = 0;
  for (int i = 0; i < 4; ++i) {
    recipe_len |= static_cast<uint32_t>(packed[t + i]) << (8 * i);
    blob_len |= static_cast<uint32_t>(packed[t + 4 + i]) << (8 * i);
  }
  if (recipe_len < 1 || static_cast<size_t>(recipe_len) > t) return false;
  if (static_cast<size_t>(blob_len) > t - recipe_len) return false;

  const std::span<const uint8_t> blob = packed.first(blob_len);
  const std::span<const uint8_t> skeleton =
      packed.subspan(blob_len, t - recipe_len - blob_len);
  const std::span<const uint8_t> recipe = packed.subspan(t - recipe_len, recipe_len);

  size_t rp = 0;
  const bool urlsafe = (recipe[rp++] & 1u) != 0;

  out.clear();
  out.reserve(skeleton.size() + blob.size() * 4 / 3 + 4);

  size_t blob_at = 0;
  size_t prev = 0;
  size_t skel_pos = 0;
  std::vector<uint8_t> enc;
  while (rp < recipe.size()) {
    uint64_t delta = 0, packed_len = 0;
    if (!get_varint(recipe, rp, delta)) return false;
    if (!get_varint(recipe, rp, packed_len)) return false;
    const size_t core_len = static_cast<size_t>(packed_len / 4);
    const size_t pad = static_cast<size_t>(packed_len % 4);
    if (pad > 2 || core_len == 0 || core_len % 4 == 1) return false;
    if (delta > skeleton.size() || prev + delta > skeleton.size()) return false;
    const size_t at = prev + static_cast<size_t>(delta);
    prev = at;
    const size_t need = core_len * 3 / 4;
    if (blob_at + need > blob.size()) return false;

    enc.clear();
    encode_core(blob.subspan(blob_at, need), urlsafe, enc);
    if (enc.size() != core_len) return false;  // not the length the recipe recorded
    blob_at += need;

    if (at < skel_pos || at > skeleton.size()) return false;
    out.insert(out.end(), skeleton.begin() + skel_pos, skeleton.begin() + at);
    out.insert(out.end(), enc.begin(), enc.end());
    for (size_t q = 0; q < pad; ++q) out.push_back('=');
    skel_pos = at;
  }
  out.insert(out.end(), skeleton.begin() + skel_pos, skeleton.end());
  if (blob_at != blob.size()) return false;
  return true;
}

}  // namespace chce
