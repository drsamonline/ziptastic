// dedup.hpp — SREP-style deduplication as a Layer 0 stage.
//
// SREP's pipeline role is to remove long-range duplicates *before* compression,
// so that every backend benefits — including the ones that cannot look across a
// block boundary (BWT, PPMd). That is the gap this fills: the engine's per-block
// backend choice is its strength, and also its blind spot for cross-block
// redundancy. The coarse index inside the parser only helps the token path.
//
// The technique is SREP's; the implementation is ours (SREP's source is
// all-rights-reserved). It reuses the CoarseIndex built in Phase 6.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

// Replaces repeated runs with back-references. Returns false when nothing was
// found worth removing — the caller then compresses the bytes unchanged.
bool dedup_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);

// Rebuilds the original from a packed stream. The original length is stored in
// the stream itself, so no extra format field is needed.
bool dedup_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
