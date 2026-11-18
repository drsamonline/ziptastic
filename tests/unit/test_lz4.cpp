// test_lz4.cpp — an LZ4 frame must be recoverable from its plaintext alone.
//
// The transform stores no parse: LZ4's matcher is deterministic, so the frame is rebuilt by
// re-encoding and compared. That makes the fixture's provenance part of the test — a frame made
// by a different liblz4 version cannot be reproduced by this one, and the transform must decline
// rather than emit something that differs. Both halves are asserted here.
#include <cstring>
#include <string>
#include <vector>

#include <lz4frame.h>

#include "harness.hpp"
#include "precompressors/lz4_wrap.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> frame_of(const std::string& text, int block_size, int linked, int level) {
  LZ4F_preferences_t prefs;
  std::memset(&prefs, 0, sizeof(prefs));
  prefs.frameInfo.blockSizeID = static_cast<LZ4F_blockSizeID_t>(block_size);
  prefs.frameInfo.blockMode = linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
  prefs.frameInfo.contentSize = text.size();
  prefs.compressionLevel = level;
  std::vector<uint8_t> out(LZ4F_compressFrameBound(text.size(), &prefs));
  const size_t n = LZ4F_compressFrame(out.data(), out.size(), text.data(), text.size(), &prefs);
  if (LZ4F_isError(n)) return {};
  out.resize(n);
  return out;
}

std::string sample() {
  std::string s;
  for (int i = 0; i < 4000; ++i) {
    s += "<row><id>";
    s += std::to_string(i);
    s += "</id><name>item ";
    s += std::to_string(i);
    s += "</name></row>\n";
  }
  return s;
}

}  // namespace

// A frame this build can reproduce is stored as plaintext and rebuilt byte for byte.
TEST(a_frame_from_this_build_round_trips) {
  const std::string text = sample();
  const auto frame = frame_of(text, LZ4F_max64KB, 1, 1);
  CHECK_MSG(!frame.empty(), "could not build a fixture frame");

  std::vector<uint8_t> payload, back;
  CHECK_MSG(lz4_unpack(frame, payload), "declined a frame this build produced");
  // The payload IS the plaintext, so it is necessarily larger than a compressed frame. The
  // claim is that the recipe costs a handful of bytes — not the ~1 MB of sequence descriptors
  // that made the store-the-parse design 49.7% worse (docs/LZ4_MEASURED.md).
  CHECK_MSG(payload.size() < text.size() + 32,
            "the recipe is more than a handful of bytes on top of the plaintext");
  CHECK_MSG(lz4_repack(payload, back), "repack failed");
  CHECK_MSG(back == frame, "round trip is not byte-exact");
}

// Every framing the header can describe must be recoverable, because the header is what the
// search reads instead of guessing.
TEST(every_block_size_and_linkage_is_recoverable) {
  const std::string text = sample();
  for (int bs : {LZ4F_max64KB, LZ4F_max256KB, LZ4F_max1MB, LZ4F_max4MB}) {
    for (int linked : {0, 1}) {
      const auto frame = frame_of(text, bs, linked, 1);
      CHECK_MSG(!frame.empty(), "fixture frame failed");
      std::vector<uint8_t> payload, back;
      CHECK_MSG(lz4_unpack(frame, payload), "declined a frame it should reproduce");
      CHECK_MSG(lz4_repack(payload, back) && back == frame, "round trip is not byte-exact");
    }
  }
}

// A frame this build cannot reproduce must be declined, not approximated. Corrupting a header
// byte is the cheapest way to make an unreproducible frame: it still decodes, and no parameter
// set reproduces it.
TEST(an_unreproducible_frame_is_declined) {
  const std::string text = sample();
  auto frame = frame_of(text, LZ4F_max64KB, 1, 1);
  CHECK_MSG(!frame.empty(), "could not build a fixture frame");
  std::vector<uint8_t> payload;
  CHECK_MSG(lz4_unpack(frame, payload), "declined a frame it should reproduce");

  // Change the content-size flag in the descriptor without changing the data: the frame still
  // decodes, and no setting in the search reproduces the altered header.
  std::vector<uint8_t> tampered = frame;
  tampered[4] ^= 0x08;
  std::vector<uint8_t> ignored;
  CHECK_MSG(!lz4_unpack(tampered, ignored), "accepted a frame it cannot reproduce");
}

TEST_MAIN()
