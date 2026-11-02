// sqlite_wrap.hpp — SQLite database pages grouped by type.
//
// Measured with this engine, post-compression (docs/SQLITE_MEASUREMENT.md):
//
//   295 KB database, 72 pages    119,016 -> 113,203   -4.9%
//   3.46 MB database, 845 pages  1,468,255 -> 1,374,217   -6.4%
//
// The transform is a reordering, not a re-encoding: pages are emitted grouped by
// their B-tree page type, so pages that look alike sit next to each other. The
// recipe costs 12 bytes, because the decoder can recompute the order from the page
// type bytes it already has — no permutation table is stored.
//
// That is a smaller win than the container transforms (20-80%), and it is recorded
// as such. It is here because a database is a file class this engine had nothing
// for, and 6% on a save file is 6%.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool sqlite_available();

// Cheap pre-check: the 16-byte SQLite header magic.
bool sqlite_likely(std::span<const uint8_t> in);

// Payload: pages grouped by type | u32 page_size | u32 page_count | "SL01"
// The page count and size are fixed-width so the payload parses from the end.
bool sqlite_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool sqlite_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
