// bcj_x86.cpp — the x86 BCJ transform, ported from xz (0BSD).
#include "bcj_x86.hpp"

namespace chce {
namespace {

inline bool test86_ms_byte(uint8_t b) { return b == 0x00 || b == 0xFF; }

// The mask table from xz: which byte of the displacement was the most
// significant on the previous instruction, used to resolve overlapping
// candidates the same way the reference does.
constexpr uint32_t kMaskToBitNumber[5] = {0, 1, 2, 2, 3};

}  // namespace

bool bcj_x86_likely(std::span<const uint8_t> data) {
  if (data.size() < 4096) return false;

  // Count the opcodes the filter can act on. Real x86 code has them at well
  // over 1%; text and databases have almost none.
  size_t hits = 0;
  for (size_t i = 0; i < data.size(); ++i) {
    if (data[i] == 0xE8 || data[i] == 0xE9) ++hits;
  }
  return hits * 1000 >= data.size() * 3;  // >= 0.3%
}

void bcj_x86_transform(std::vector<uint8_t>& buf, bool encode) {
  const size_t size = buf.size();
  if (size < 5) return;

  uint32_t prev_mask = 0;
  uint32_t prev_pos = static_cast<uint32_t>(-5);
  const uint32_t now_pos = 0;  // one whole-buffer pass, so the offset is 0
  uint8_t* buffer = buf.data();

  const size_t limit = size - 5;
  size_t buffer_pos = 0;

  while (buffer_pos <= limit) {
    uint8_t b = buffer[buffer_pos];
    if (b != 0xE8 && b != 0xE9) {
      ++buffer_pos;
      continue;
    }

    const uint32_t offset =
        now_pos + static_cast<uint32_t>(buffer_pos) - prev_pos;
    prev_pos = now_pos + static_cast<uint32_t>(buffer_pos);

    if (offset > 5) {
      prev_mask = 0;
    } else {
      for (uint32_t i = 0; i < offset; ++i) {
        prev_mask &= 0x77;
        prev_mask <<= 1;
      }
    }

    b = buffer[buffer_pos + 4];

    if (test86_ms_byte(b) && (prev_mask >> 1) <= 4 && (prev_mask >> 1) != 3) {
      uint32_t src = (static_cast<uint32_t>(b) << 24) |
                     (static_cast<uint32_t>(buffer[buffer_pos + 3]) << 16) |
                     (static_cast<uint32_t>(buffer[buffer_pos + 2]) << 8) |
                     static_cast<uint32_t>(buffer[buffer_pos + 1]);

      uint32_t dest = 0;
      for (;;) {
        if (encode) {
          dest = src + (now_pos + static_cast<uint32_t>(buffer_pos) + 5);
        } else {
          dest = src - (now_pos + static_cast<uint32_t>(buffer_pos) + 5);
        }

        if (prev_mask == 0) break;

        const uint32_t i = kMaskToBitNumber[prev_mask >> 1];
        b = static_cast<uint8_t>(dest >> (24 - i * 8));
        if (!test86_ms_byte(b)) break;

        src = dest ^ ((1u << (32 - i * 8)) - 1);
      }

      buffer[buffer_pos + 4] =
          static_cast<uint8_t>(~((((dest >> 24) & 1) - 1)));
      buffer[buffer_pos + 3] = static_cast<uint8_t>(dest >> 16);
      buffer[buffer_pos + 2] = static_cast<uint8_t>(dest >> 8);
      buffer[buffer_pos + 1] = static_cast<uint8_t>(dest);
      buffer_pos += 5;
      prev_mask = 0;
    } else {
      ++buffer_pos;
      prev_mask |= 1;
      if (test86_ms_byte(b)) prev_mask |= 0x10;
    }
  }
}

}  // namespace chce
