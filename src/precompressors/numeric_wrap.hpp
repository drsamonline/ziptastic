// numeric_wrap.hpp — integer CSV/TSV columns coded as delta + zigzag + varint.
//
// A text CSV of integers stores each value as decimal characters. The same data
// as delta-coded varints is far smaller, and it compresses further because the
// byte stream is regular. Measured on a four-column integer CSV: 163,616 bytes as
// text, 39,108 after this transform, both post-compression — -76.1%.
//
// What the measurements settled (docs/NUMERIC_RESIDUAL_EXPERIMENT.md):
//   * integers only. Floats gave -0.6% on smooth data, which is not worth the
//     gating complexity. Gorilla-style leading-zero trimming was worse than plain
//     XOR, and Rice coding worse than varint: bit-level encodings lose to
//     byte-aligned ones against a strong backend.
//   * delta only. Linear and quadratic predictors lost to plain delta in every
//     column tested, which also removes the need for a per-column predictor ID.
//   * per-column accept/reject. The trial declined on random and noisy columns
//     exactly as intended; that is what makes it safe to apply automatically.
//
// Exactness is the whole contract: every cell must parse, re-serialise, and
// byte-compare equal. A column with "007", "+5", "5.0" or mixed formatting is
// declined rather than normalised.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool numeric_available();

// Cheap pre-check: does this look like delimited text with at least two rows?
bool numeric_likely(std::span<const uint8_t> in);

bool numeric_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool numeric_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
