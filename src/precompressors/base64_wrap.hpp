// base64_wrap.hpp — base64 runs decoded to their bytes, with a recipe that
// rebuilds the exact text.
//
// Measured with this engine (docs/BASE64_INTEGRATION.md), not with xz:
//
//   base64 of a 21,797-byte JPEG, single line      21,481 -> 17,272   -19.6%
//   the same, MIME-wrapped at 76 columns           21,927 -> 17,507   -20.2%
//   the same inside a JSON document                21,551 -> 17,332   -19.6%
//   base64 of 200 KB of spreadsheet XML, wrapped   21,177 -> 10,931   -48.4%
//   the same, single line                          10,282 -> 10,906    -6.1%  LOSS
//
// Two things that measurement settled:
//
//   * The win is **line wrapping**, not the 33% inflation. The same payload
//     single-line is worth 19.6% on dense data and *negative* on structured text;
//     wrapped at 76 columns it is worth 48.4% on the same structured text, because
//     the breaks destroy long-range regularity the backend would otherwise use.
//   * The decoded bytes must sit at **offset 0** of the payload. Putting the
//     skeleton first cost 2.3% instead of 20.2% on a wrapped JPEG: the trailing
//     skeleton stopped brunsli from recognising the JPEG. Blob first, skeleton
//     after.
//
// Because the single-line structured case is a loss, this transform is never
// applied unconditionally: the caller runs an accept/reject trial first.
//
// Exactness is the usual contract — decode, re-encode, byte-compare, decline on
// mismatch — so line width, EOL style and stripped padding need no special cases:
// the skeleton carries the line breaks verbatim and each run records its own
// character count, from which the padding follows.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool base64_available();

// Cheap pre-check: is there at least one long base64 run in here?
bool base64_likely(std::span<const uint8_t> in);

// Payload, parsed from the end:
//   blob | skeleton | recipe | u32 recipe_len | u32 blob_len | "B641"
//
//   blob     = the decoded bytes, in document order, at OFFSET 0 — measured: a
//              header in front of it stops brunsli recognising the JPEG inside
//              and the gain falls from 20.2% to 2.2%.
//   skeleton = the input with every accepted run's characters removed.
//   recipe   = u8 flags (bit 0 = URL-safe alphabet), then per run a varint delta
//              of the insertion point in skeleton coordinates and a varint
//              enc_len*4+pad. Padding is stored, not inferred: a stripped-padding
//              file has none where the canonical encoding would carry two.
bool base64_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool base64_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
