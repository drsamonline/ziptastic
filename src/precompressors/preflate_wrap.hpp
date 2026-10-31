// preflate_wrap.hpp — DEFLATE recompression as a Layer 0 transform.
//
// A ZIP, PNG or JAR is already DEFLATE-compressed, so compressing it directly is
// compressing compressed data. Preflate parses the DEFLATE streams, inflates
// them, and records a small correction that lets the exact original bytes be
// recreated — so the inflated data (highly compressible) goes through our
// pipeline instead, and the correction rides along.
//
// Vendored source: vendor/hcs/old/preflate-master (Apache-2.0, verified).
//
// The interface takes vectors rather than spans because this wrapper is built as
// C++17 (preflate's headers predate C++20) while the rest of the engine is C++20.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

// True when this build has preflate linked in.
bool preflate_available();

// Finds DEFLATE streams in `input` (a ZIP container, or one bare DEFLATE stream),
// inflates them, and returns a single payload that our compressor can code:
//
//   varint stream_count
//   per stream: varint offset, varint comp_len, varint infl_len, varint diff_len
//   per stream: diff bytes
//   per stream: inflated bytes
//   skeleton (the original bytes with the streams cut out)
//
// Returns false when there is nothing to gain or the input does not parse — the
// caller then compresses the bytes as they are.
bool preflate_unpack(const std::vector<uint8_t>& input, std::vector<uint8_t>& payload);

// Rebuilds the original from a payload produced above. Returns false rather than
// producing bytes it is not sure about.
bool preflate_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& original);

}  // namespace chce
