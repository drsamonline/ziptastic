// wav_wrap.hpp — 16-bit PCM WAV data coded as per-channel deltas.
//
// Measured with this engine, post-compression (docs/WAV_MEASUREMENT.md):
//
//   tonal music, 44.1 kHz stereo    554,665 -> 463,411   -16.5%
//   noise-heavy SFX                  399,987 -> 391,975    -2.0%
//   quiet ambience                   408,099 -> 398,791    -2.3%
//
// The win is where the signal is predictable, which is why the transform is gated
// by the same accept/reject trial as base64 rather than applied to every WAV.
//
// Per-channel delta, not mid/side: mid/side measured slightly worse on all three
// fixtures (469,375 and 397,201 and 404,343), so the simpler form ships.
//
// Exactness: delta + zigzag varint over int16 samples is exactly invertible, and
// the payload is re-encoded and byte-compared before it is returned. Anything the
// parser does not recognise — float samples, more than 8 channels, a truncated
// data chunk, an odd byte count — declines.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool wav_available();

// Cheap pre-check: RIFF/WAVE with a 16-bit PCM fmt chunk.
bool wav_likely(std::span<const uint8_t> in);

// Payload, parsed from the end like base64's:
//   pcm-stream | head | tail | u8 channels | varint tail_len | varint head_len
//              | u32 stream_len | "WV01"
//
// `head` is everything up to the first byte of sample data and `tail` everything
// after it, both verbatim — a WAV can carry LIST/INFO chunks, and those are the
// caller's bytes. `pcm-stream` is one zigzag varint per sample, in frame order,
// delta-coded per channel.
bool wav_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool wav_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
