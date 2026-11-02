# WAV sample data as per-channel deltas — shipped

Written 2026-11-02. Measured before any C++ was written, then again through the
shipped CLI.

## Result

| file | before | after | gain | `layers` |
|---|---|---|---|---|
| tonal music, 44.1 kHz stereo, 200k frames | 554,665 | **463,477** | **−16.4%** | 0x80 |
| noise-heavy SFX (explosion-shaped) | 399,987 | 391,996 | −2.0% | 0x80 |
| quiet ambience | 408,099 | 398,977 | −2.2% | 0x80 |

All round-trip exact. `layers` 0x80 is `kLayerWav`, the last free layer bit.

## What the measurement settled

**Per-channel delta beats mid/side.** The prototype measured both on the same
three fixtures: mid/side gave 469,375 / 397,201 / 404,343 against 463,411 / 391,975
/ 398,791 for plain per-channel delta. The simpler form wins on all three, so it
ships and mid/side does not.

**The gain is where the signal is predictable.** 16.4% on tonal material, 2% on
noise. That is not a bug in the transform — noise has no local structure to remove —
and it is why the transform goes through the same accept/reject trial as base64
(`base64_pays`, which runs the real pipeline on a 256 KiB sample of each side)
rather than being applied to every WAV.

## Format

```
pcm-stream | head | tail | u32 stream_len | u32 head_len | u32 tail_len
           | u8 channels | u8 reserved | "WV01"
```

`head` is everything before the sample data and `tail` everything after it, both
verbatim — a WAV can carry `LIST`/`INFO` chunks and those are the caller's bytes.
`pcm-stream` is one zigzag varint per sample in frame order, delta-coded against the
previous sample in the same channel. The trailer is fixed-width so it can be parsed
from the end; the sample count is recovered by walking the varints.

## What declines

`fmt ` must be format 1 (PCM) with 16-bit samples and 1–8 channels; the `data` chunk
must be a whole number of frames and must fit inside the file. Float, 24-bit, ADPCM
and RF64 all decline — they are different transforms, not this one — and a truncated
data chunk declines rather than being approximated.

## The bit budget is now exhausted

`kLayerWav` is bit 7. **Every layer bit is allocated.** The next Layer 0 capability
needs a second `layers` byte and a format version bump, which is a change to plan as
an item of its own rather than discover halfway through the next transform.

## Files

- `src/precompressors/wav_wrap.{hpp,cpp}` — the transform and its inverse.
- `include/chimera/format.hpp` — `kLayerWav` (0x80).
- `src/engine/compressor.cpp` — the stage and the trial.
- `src/engine/decompressor.cpp` — undone next to base64.
- `src/engine/compressor.hpp`, `src/main.cpp` — `opt.wav`, `--no-wav`.
- `tests/unit/test_wav.cpp` — 6 cases.
