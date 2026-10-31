// bcj_x86.hpp — the x86 BCJ (Branch/Call/Jump) filter as a Layer-0 transform.
//
// x86 code is full of relative CALL/JMP displacements. Each one is a different
// number, so the match finder and the literal model both see noise where the
// structure is actually regular. The BCJ filter rewrites those displacements
// from relative to absolute, which makes them repeat, and is exactly invertible.
//
// Adapted from xz's src/liblzma/simple/x86.c (0BSD, authors Igor Pavlov and
// Lasse Collin) into our own tree, as the manifest's "adapt, don't link" rule
// requires. The algorithm is unchanged; what is new is the container and the
// whole-buffer calling convention.
//
// Note: `xz -9` does *not* apply this unless asked (`xz --x86`), so on
// executable data this is a legitimate win against our own baseline.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

// True when the buffer looks like x86 code, judged by the density of the
// CALL/JMP opcodes the filter operates on. Cheap, and deliberately
// conservative: a wrong guess costs a little ratio, never correctness, because
// the transform is invertible whatever the input.
bool bcj_x86_likely(std::span<const uint8_t> data);

// Applies the filter in place. `encode` and `decode` are exact inverses for any
// buffer, which is what makes it safe to apply without trial encoding.
void bcj_x86_transform(std::vector<uint8_t>& buf, bool encode);

}  // namespace chce
