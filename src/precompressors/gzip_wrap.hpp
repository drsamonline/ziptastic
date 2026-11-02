// gzip_wrap.hpp — gzip members inflated, with the exact bytes reproducible.
//
// precomp reaches ZIP and PDF but not gzip, and `.tar.gz` is the most common
// container there is. A gzip member is a 10-byte header, one bare DEFLATE stream,
// and an 8-byte CRC-32/ISIZE trailer — so the DEFLATE half is exactly what
// preflate already does for ZIP members, and the new work is only the framing.
//
// Multi-member gzip is the reason this is a container transform rather than a
// preflate call: a `.tar.gz` produced by `gzip` has one member, but a log rotated
// with `cat a.gz b.gz > all.gz` has many, and the format says a decoder must read
// them all.
//
// Exactness is verified on the input before the payload is returned: the members
// are re-emitted from the stored parse and byte-compared, exactly as preflate's
// own container layer does it.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool gzip_available();

// True when the buffer starts with a gzip member magic (1f 8b, method 8).
bool gzip_likely(const std::vector<uint8_t>& in);

// Payload:
//   "GZ01" | varint members
//          | per member: varint header_len, varint diff_len, varint infl_len
//          | all member headers, verbatim
//          | all preflate diffs
//          | all inflated bytes
//          | all 8-byte trailers, verbatim
//
// The headers and trailers are stored rather than reconstructed: mtime, original
// name, extra fields and the OS byte are the caller's bytes, not ours to guess.
bool gzip_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

// Rebuilds the original gzip file. Returns false rather than producing bytes it
// is not sure about.
bool gzip_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
