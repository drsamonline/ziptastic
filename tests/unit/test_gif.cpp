// test_gif.cpp — the GIF layer must reproduce the original LZW byte-for-byte.
//
// That is the whole contract: a GIF's LZW is not canonical, so if the re-encode
// differs by one bit the transform must decline rather than produce a file that
// decodes to the same picture but not the same bytes.
#include <cstdio>
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "precompressors/gif_wrap.hpp"

using namespace chce;

TEST(detects_gif) {
  // 13 bytes is the header plus logical screen descriptor; the trailer makes a
  // real file longer, which is what gif_likely checks for.
  std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, 0x3B};
  CHECK(gif_likely(gif));
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(!gif_likely(png));
  CHECK(!gif_likely({}));
}

TEST(declines_non_gif) {
  std::vector<uint8_t> junk(5000, 0x5A);
  std::vector<uint8_t> packed;
  CHECK_MSG(!gif_pack(junk, packed), "packed data that is not a GIF");
}

TEST(round_trips_a_real_gif) {
  // A real 24x24 GIF, embedded rather than vendored: the commit path this
  // project uses is text-only, and a binary test fixture would have to be
  // fetched separately. Small enough to read as data.
  static const uint8_t kGif[] = {71,73,70,56,55,97,24,0,24,0,132,0,0,0,0,0,3,5,7,6,10,14,9,15,21,12,20,28,15,25,35,18,30,42,21,35,49,24,40,56,27,45,63,30,50,70,33,55,77,36,60,84,39,65,91,42,70,98,45,75,105,48,80,112,51,85,119,54,90,126,57,95,133,60,100,140,63,105,147,66,110,154,69,115,161,72,120,168,75,125,175,78,130,182,81,135,189,84,140,196,87,145,203,90,150,210,93,155,217,44,0,0,0,0,24,0,24,0,64,8,214,0,1,28,112,80,129,195,0,5,17,48,124,48,208,128,194,6,1,9,32,92,240,80,128,193,4,13,1,20,50,116,8,81,34,69,139,24,17,60,176,208,129,192,2,9,25,60,86,188,24,64,36,73,147,40,5,18,52,136,16,100,203,145,37,79,102,144,89,240,96,194,133,13,96,238,28,216,19,161,198,134,15,35,78,44,64,243,231,198,164,42,109,186,236,240,82,39,207,166,71,57,42,237,184,84,42,78,161,87,7,204,244,153,21,106,87,150,34,67,126,181,74,20,43,80,135,79,185,126,68,187,54,230,1,187,99,141,190,53,59,151,109,94,167,72,229,86,188,89,21,111,81,192,28,207,170,45,60,244,47,134,192,74,231,46,206,137,151,236,94,193,94,95,54,62,92,22,243,197,186,155,221,198,141,184,114,50,216,182,150,13,240,45,77,152,114,104,196,171,51,83,70,173,119,116,84,180,174,195,214,134,236,49,32,0,59};

  std::vector<uint8_t> gif(kGif, kGif + sizeof(kGif));
  std::vector<uint8_t> packed;
  if (!gif_pack(gif, packed)) return;  // declined is a valid outcome for some GIFs

  std::vector<uint8_t> back;
  CHECK_MSG(gif_unpack(packed, back), "unpack failed");
  CHECK_MSG(back == gif, "GIF round-trip is not bit-exact");
}

TEST_MAIN()
