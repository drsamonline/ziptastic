# Validate before allocate

Written 2026-11-01, same session as the per-block integrity work. This is the
third item from the lexora assessment and the same family as the second: both are
about what a decoder is allowed to believe.

## The failure mode

The decoder sized its per-block structures from the header alone:

```cpp
std::vector<Blk> blks(static_cast<size_t>(fh.block_count));   // parallel path
std::vector<std::vector<uint8_t>> parts(blks.size());
```

`fh.block_count` was bounded only by `original_size`, which is bounded by 1 TiB.
So an 83-byte stream could ask for ten million block entries. Measured sizes on
this machine: `sizeof(BlockHeader)` 56, `sizeof(Blk)` 72, `sizeof(std::vector)`
24 — so that header asks for **686 MB plus 228 MB, about 914 MB**, on a 2 GiB
container, before a single byte of payload is looked at.

This is the same shape as the desync bugs this project kept finding: an
unvalidated size field turning into a `bad_alloc` somewhere far from the parse
that should have refused it. The fix is not a better error handler at the
allocation site. It is refusing the header where it is read.

## The fix: a structural bound, not a heuristic

A block costs at least `kMinBlockHeaderBytes` = 35 bytes on disk — a mode byte,
two varints of at least one byte each, and the 32-byte digest. So:

> The bytes actually in hand are a hard upper bound on how many blocks a stream
> can contain.

That is a fact about the input, which is exactly the kind of thing this project
prefers to a prediction (three predictors have now been wrong here, all in the
direction that costs ratio). No ratio model, no sample, no estimate:

```cpp
// in decompress(), immediately after read_file_header
if (fh.block_count > r.remaining() / kMinBlockHeaderBytes) {
  return Err::BadBlockSize;
}
```

And the cheap half in `read_file_header` itself, so a header-only read (a probe,
a test) cannot accept an absurd count either: `block_count > kMaxBlocks`
(16.7M — 64 GB at 4 KiB blocks, 64 TiB at the 4 MiB default) is `bad-block-size`.

The bound lives in the decoder rather than in `read_file_header` because only the
decoder knows how long the stream is. Putting it in the header parser would have
broken every legitimate header-only caller, which is how these checks end up
disabled.

## Measured

| | |
|---|---|
| crafted stream: 83 bytes declaring 10,000,000 blocks | rejected in **13 ms** (process start included) |
| what it would have allocated before the fix | 686 MB + 228 MB ≈ 914 MB |
| error | `bad-block-size` → `ziptastic: decompression failed: invalid block size` |
| normal streams | byte-identical output, 73,436 for `book.xlsx` before and after |

No format change and no ratio cost: this item only decides what the decoder
refuses.

## Tests

In `tests/unit/test_roundtrip.cpp`:

- `impossible_block_count_is_rejected_before_allocating` — a header declaring a
  million blocks with 40 bytes behind it must return `bad-block-size`, not try.
- `block_count_above_the_ceiling_is_rejected` — `kMaxBlocks + 1` is refused by
  `read_file_header` on its own.
- `a_legitimate_multi_block_stream_still_decodes` — 13 real blocks, so the bound
  cannot be so tight that real streams fail.

## What this does not cover

The initial `dst.reserve(min(original_size, 256 MiB))` still trusts the header up
to 256 MiB. That is a deliberate, documented cap rather than a structural bound,
and shrinking it further would mean predicting compression ratio. If it ever
matters, the honest fix is the same one used here: find a fact about the bytes in
hand, not a better estimate.

`dedup_unpack` caps its declared output at 1 TiB, which is the same class of
weakness — a crafted payload could ask for a 1 TiB reserve. It needs a stream that
already decoded cleanly to reach, so it is behind the per-block digests now, but
it should get the same treatment as this item when the Layer 0 chain is next
touched.
