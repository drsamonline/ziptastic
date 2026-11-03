> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# Format v6: the header now says how big the answer is

Written 2026-11-03. One field, and it closes the largest hole the audit found.

## The change

```
"CHCE" | version(6) | flags | layers | layers2 | original_size | true_size | block_count | crc32
                                                        ^^^^^^^^^
```

- **`original_size`** — unchanged: the size *after* Layer 0, i.e. what the block loop encodes.
- **`true_size`** — new: the size the caller actually gets back.

Both are varints, so the cost is one byte for any file under 128 bytes and two or three
above that. On a 4.7 MB stream that is three bytes.

## Why it was needed

The decoder had no way to know the final size, so three things were heuristics:

1. **The expansion ceiling.** `AUDIT_FINDINGS.md` B3: the inverse chain expands, and the only
   guard was `4096 × original_size + 1 MiB`, checked after every stage. It is still there as
   the *intermediate* bound, but the final check is now exact.
2. **`DecompressReport::original_size`** had to be redefined mid-audit because the header
   carried the wrong number for transformed streams.
3. **A truncated stream** could only be detected by the CRC-32 at the end, after the work.

## The bug this change introduced, and what it taught

The first version used `true_size` as the intermediate ceiling too, on the reasoning that the
inverse chain only grows toward the final size. **That reasoning is wrong**, and the container
tests caught it immediately: a 50 KB `.tar.gz` inflates to a 300 KB tar, then to a larger
inflated ZIP, and only then do the nested transforms shrink it to the original 50 KB.

The chain goes **up and comes back down**. So:

- intermediates get a ceiling well above the final (`true_size × 4096 + 1 MiB`),
- the last comparison is `== true_size`, exact.

Two container tests failed on the first build and none after the correction — which is the
argument for having them.

## Compatibility

v5 streams are refused with `bad-version`, as every earlier version has been. There is no
migration path because there is one user and no archive worth migrating; the CLI prints the
format version (`ziptastic version` → `CHCE format v6`) so a mismatch is never a mystery.

## What is still a heuristic

The intermediate ceiling. A crafted stream can still make the decoder build up to
`4096 × true_size` bytes before the final check fails — but `true_size` is now bounded by
`kMaxStreamSize` (1 TiB) *and* by the caller's own file, so the practical worst case is
bounded by the input rather than by a number the attacker chose freely. The remaining fix, if
it ever matters, is a per-stage declared size; nothing here needs it yet.
