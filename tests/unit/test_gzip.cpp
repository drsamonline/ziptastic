// test_gzip.cpp — gzip members must be reproducible byte-for-byte.
//
// The DEFLATE half is preflate's, already covered by its own tests. What is new
// here is the framing: headers with names and timestamps, multi-member files, and
// the trailer, all of which have to come back exactly as they went in.
//
// The fixtures are embedded as byte arrays rather than checked in as files: the
// commit path is text-only UTF-8 and would silently skip a binary.
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/gzip_wrap.hpp"

using namespace chce;

namespace {

const uint8_t kMemberA[] = {
  0x1f, 0x8b, 0x08, 0x08, 0x00, 0xf1, 0x53, 0x65, 0x02, 0xff, 0x73, 0x68,
  0x65, 0x65, 0x74, 0x2e, 0x78, 0x6d, 0x6c, 0x00, 0xb3, 0x29, 0xca, 0x2f,
  0xb7, 0xb3, 0x49, 0x56, 0x28, 0xb2, 0x55, 0x72, 0x34, 0x54, 0xb2, 0xb3,
  0x29, 0xb3, 0x33, 0x34, 0x30, 0x30, 0xb6, 0xd1, 0x2f, 0xb3, 0xb3, 0xd1,
  0x4f, 0x06, 0x62, 0x90, 0x34, 0x97, 0xcd, 0xa8, 0xa2, 0x51, 0x45, 0xa3,
  0x8a, 0x46, 0x15, 0x8d, 0x2a, 0x1a, 0xac, 0x8a, 0x00, 0x5b, 0x86, 0x6f,
  0xaa, 0xc8, 0x05, 0x00, 0x00
};
const size_t kMemberA_len = sizeof(kMemberA);

const uint8_t kMemberB[] = {
  0x1f, 0x8b, 0x08, 0x08, 0x01, 0xf1, 0x53, 0x65, 0x02, 0xff, 0x73, 0x65,
  0x63, 0x6f, 0x6e, 0x64, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x2b, 0x4e, 0x4d,
  0xce, 0xcf, 0x4b, 0x51, 0xc8, 0x4d, 0xcd, 0x4d, 0x4a, 0x2d, 0xd2, 0x51,
  0x00, 0x72, 0x92, 0x13, 0x4b, 0x52, 0xf3, 0x80, 0x38, 0x45, 0x21, 0x31,
  0xad, 0x24, 0xb5, 0x48, 0xa1, 0x24, 0x23, 0x55, 0x21, 0x2d, 0xb3, 0xa8,
  0xb8, 0x84, 0xab, 0x78, 0x54, 0xed, 0xa8, 0xda, 0x51, 0xb5, 0x14, 0xa9,
  0x05, 0x00, 0x1a, 0x04, 0x5a, 0x0d, 0x70, 0x03, 0x00, 0x00
};
const size_t kMemberB_len = sizeof(kMemberB);

std::vector<uint8_t> vec(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

}  // namespace

TEST(round_trips_one_member) {
  const auto in = vec(kMemberA, kMemberA_len);
  std::vector<uint8_t> payload, back;
  CHECK_MSG(gzip_unpack(in, payload), "declined a plain gzip member");
  CHECK_MSG(payload.size() > in.size(), "payload should hold the inflated bytes");
  CHECK_MSG(gzip_repack(payload, back), "repack failed");
  CHECK_MSG(back == in, "single-member round-trip is not byte-exact");
}

// A rotated log, or anything made by `cat a.gz b.gz > all.gz`: the format requires
// a decoder to read every member, and a transform that only handled the first one
// would silently lose the rest.
TEST(round_trips_multiple_members) {
  std::vector<uint8_t> in = vec(kMemberA, kMemberA_len);
  in.insert(in.end(), kMemberB, kMemberB + kMemberB_len);

  std::vector<uint8_t> payload, back;
  CHECK_MSG(gzip_unpack(in, payload), "declined a two-member gzip");
  CHECK_MSG(gzip_repack(payload, back), "repack failed");
  CHECK_MSG(back == in, "multi-member round-trip is not byte-exact");
}

TEST(the_payload_carries_the_inflated_bytes) {
  const auto in = vec(kMemberA, kMemberA_len);
  std::vector<uint8_t> payload;
  CHECK_MSG(gzip_unpack(in, payload), "declined a plain gzip member");
  // The row markup the fixture was built from must be present verbatim.
  const char* needle = "<row><c r=\"A1\"><v>1003</v></c></row>";
  const std::string hay(payload.begin(), payload.end());
  CHECK_MSG(hay.find(needle) != std::string::npos,
            "inflated bytes are not in the payload");
}

TEST(declines_non_gzip) {
  const std::vector<uint8_t> not_gzip = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                         13, 14, 15, 16, 17, 18, 19, 20};
  std::vector<uint8_t> payload;
  CHECK_MSG(!gzip_unpack(not_gzip, payload), "accepted non-gzip data");
}

TEST(declines_truncated_member) {
  auto in = vec(kMemberA, kMemberA_len);
  in.resize(in.size() - 4);  // cut into the trailer
  std::vector<uint8_t> payload;
  CHECK_MSG(!gzip_unpack(in, payload), "accepted a truncated gzip");
}

// A member followed by bytes that are not a member: the transform must decline the
// whole file rather than transform the part it understood.
TEST(declines_trailing_rubbish) {
  auto in = vec(kMemberA, kMemberA_len);
  for (int i = 0; i < 12; ++i) in.push_back(0xAA);
  std::vector<uint8_t> payload;
  CHECK_MSG(!gzip_unpack(in, payload), "accepted a gzip with trailing rubbish");
}

TEST(repack_rejects_a_damaged_payload) {
  const auto in = vec(kMemberA, kMemberA_len);
  std::vector<uint8_t> payload, back;
  CHECK_MSG(gzip_unpack(in, payload), "declined a plain gzip member");

  auto truncated = payload;
  truncated.resize(truncated.size() - 3);
  CHECK_MSG(!gzip_repack(truncated, back), "accepted a truncated payload");

  auto wrong_magic = payload;
  wrong_magic[0] = 'X';
  CHECK_MSG(!gzip_repack(wrong_magic, back), "accepted a wrong magic");
}

TEST(unpacking_is_deterministic) {
  const auto in = vec(kMemberA, kMemberA_len);
  std::vector<uint8_t> a, b;
  CHECK_MSG(gzip_unpack(in, a), "declined a plain gzip member");
  CHECK_MSG(gzip_unpack(in, b), "declined a plain gzip member");
  CHECK_MSG(a == b, "unpacking the same input twice gave different payloads");
}

TEST_MAIN()
