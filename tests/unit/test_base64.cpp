// test_base64.cpp — base64 runs must be exact or declined.
//
// The contract is the same as every other Layer 0 transform: every accepted input
// round-trips byte-for-byte, and every layout that cannot be reproduced exactly is
// declined rather than normalised. MIME wrapping, CRLF, stripped padding and the
// URL-safe alphabet all have to come back character for character.
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/base64_wrap.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::string str(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

const char* kStd =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Deterministic payload whose bytes are not all the same, so a transform that
// quietly reordered or truncated something would show up in the comparison.
std::string payload(size_t n) {
  std::string s;
  s.resize(n);
  uint32_t x = 0x9e3779b9u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = static_cast<char>(x >> 24);
  }
  return s;
}

std::string b64(const std::string& raw) {
  std::string out;
  size_t i = 0;
  for (; i + 3 <= raw.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) |
                       (static_cast<uint8_t>(raw[i + 1]) << 8) |
                       static_cast<uint8_t>(raw[i + 2]);
    out.push_back(kStd[(v >> 18) & 63]);
    out.push_back(kStd[(v >> 12) & 63]);
    out.push_back(kStd[(v >> 6) & 63]);
    out.push_back(kStd[v & 63]);
  }
  const size_t rem = raw.size() - i;
  if (rem == 1) {
    const uint32_t v = static_cast<uint32_t>(static_cast<uint8_t>(raw[i])) << 16;
    out.push_back(kStd[(v >> 18) & 63]);
    out.push_back(kStd[(v >> 12) & 63]);
    out += "==";
  } else if (rem == 2) {
    const uint32_t v = (static_cast<uint32_t>(static_cast<uint8_t>(raw[i])) << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(raw[i + 1])) << 8);
    out.push_back(kStd[(v >> 18) & 63]);
    out.push_back(kStd[(v >> 12) & 63]);
    out.push_back(kStd[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::string wrap(const std::string& s, size_t width, const char* eol) {
  std::string out;
  for (size_t i = 0; i < s.size(); i += width) {
    out += s.substr(i, width);
    out += eol;
  }
  return out;
}

void round_trip(const std::string& text, const char* what) {
  std::vector<uint8_t> packed;
  CHECK_MSG(base64_pack(bytes(text), packed), what);
  std::vector<uint8_t> back;
  CHECK_MSG(base64_unpack(packed, back), "unpack failed");
  CHECK_MSG(str(back) == text, "round-trip is not byte-exact");
}

}  // namespace

TEST(round_trips_single_line) { round_trip(b64(payload(3000)), "declined a single-line file"); }

TEST(round_trips_mime_wrapping) {
  round_trip(wrap(b64(payload(3000)), 76, "\n"), "declined a MIME-wrapped file");
}

TEST(round_trips_crlf_wrapping) {
  round_trip(wrap(b64(payload(3000)), 76, "\r\n"), "declined a CRLF-wrapped file");
}

// Padding stripped is a real variant: JWTs and several APIs drop it.
TEST(round_trips_stripped_padding) {
  std::string s = b64(payload(3000));
  while (!s.empty() && s.back() == '=') s.pop_back();
  round_trip(s, "declined stripped padding");
}

TEST(round_trips_url_safe_alphabet) {
  std::string s = b64(payload(3000));
  for (char& c : s) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  round_trip(s, "declined the URL-safe alphabet");
}

// The real-world shape: a document with an embedded blob and text around it.
TEST(round_trips_embedded_in_a_document) {
  const std::string blob = b64(payload(2000));
  const std::string doc =
      "{\n  \"name\": \"photo.jpg\",\n  \"encoding\": \"base64\",\n  \"data\": \"" + blob +
      "\",\n  \"note\": \"trailing text stays put\"\n}\n";
  round_trip(doc, "declined an embedded blob");
}

TEST(round_trips_several_runs) {
  const std::string a = b64(payload(600));
  const std::string b = b64(payload(700));
  const std::string text = "first: " + a + " middle text " + b + " end\n";
  round_trip(text, "declined multiple runs");
}

TEST(declines_prose) {
  const std::string text =
      "This paragraph talks about base64 at some length, but it carries no encoded "
      "payload of any kind, so there is nothing here to decode and re-encode.";
  std::vector<uint8_t> packed;
  CHECK_MSG(!base64_pack(bytes(text), packed), "accepted prose as base64");
}

TEST(declines_short_runs) {
  const std::string text = "id: " + b64(payload(6)) + "\n";  // 8 chars, below the floor
  std::vector<uint8_t> packed;
  CHECK_MSG(!base64_pack(bytes(text), packed), "accepted a run too short to be worth it");
}

TEST(decoder_rejects_damaged_payload) {
  std::vector<uint8_t> packed;
  CHECK_MSG(base64_pack(bytes(b64(payload(3000))), packed), "declined a clean file");

  std::vector<uint8_t> truncated(packed.begin(), packed.end() - 3);
  std::vector<uint8_t> back;
  CHECK_MSG(!base64_unpack(truncated, back), "accepted a truncated payload");

  // The magic lives in the trailer, not at the front: the blob has to start at
  // offset 0 so downstream transforms still recognise it.
  std::vector<uint8_t> wrong_magic = packed;
  wrong_magic[wrong_magic.size() - 1] = 'X';
  CHECK_MSG(!base64_unpack(wrong_magic, back), "accepted a wrong magic");

  std::vector<uint8_t> extra = packed;
  extra.push_back(0);
  CHECK_MSG(!base64_unpack(extra, back), "accepted trailing bytes");
}

TEST(packing_is_deterministic) {
  const std::string text = wrap(b64(payload(2000)), 64, "\n");
  std::vector<uint8_t> a, b;
  CHECK_MSG(base64_pack(bytes(text), a), "declined a clean file");
  CHECK_MSG(base64_pack(bytes(text), b), "declined a clean file");
  CHECK_MSG(a == b, "packing the same input twice gave different bytes");
}

// The payload must start with the decoded bytes: that is what lets brunsli and
// preflate still recognise what the blob is. Skeleton-first measured 2.3% where
// blob-first measured 20.2%, so this is a load-bearing property, not a detail.
TEST(the_blob_comes_first_in_the_payload) {
  const std::string raw = payload(3000);
  std::vector<uint8_t> packed;
  CHECK_MSG(base64_pack(bytes(b64(raw)), packed), "declined a clean file");
  CHECK_MSG(packed.size() > raw.size(), "payload smaller than its own blob");
  CHECK_MSG(std::memcmp(packed.data(), raw.data(), raw.size()) == 0,
            "the decoded blob does not start at the front of the payload");
}

TEST_MAIN()
