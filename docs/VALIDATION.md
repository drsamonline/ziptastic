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


## The last instance, fixed

`dedup_unpack` was documented here as the one remaining place that trusted a
self-declared size: it capped the declared output at 2^40 and then reserved exactly that,
so a five-byte payload could ask for a **1 TiB allocation before the first command was
read**.

Fixed the same way as `block_count`: the reserve is now bounded by what the bytes in hand
justify — `packed.size() * 32 + 4096` — and the vector grows as output is actually
produced. Growth is amortised, so a legitimate large expansion costs nothing extra, and an
illegitimate one never gets memory it has not earned.

`a_huge_declared_size_does_not_allocate` in `tests/unit/test_dedup.cpp` pins it: a stream
declaring 2^40 with no commands must be refused, produce nothing, and be refused
**quickly** — a 1 TiB reserve either throws or takes long enough to notice, so the test
fails on time as well as on the return value.

With that, no decoder path allocates from a number the stream declares about itself.

## The fuzz test was not fuzzing the transforms

`tests/unit/test_fuzz.cpp` has mutated 1,000 streams since it was written — but it built
them with **default options**, and `CompressOptions::precomp` defaults to `false`. So no
Layer 0 transform ever ran in it, and **not one transform inverse function was being
fuzzed**. Those are the parsers that read attacker-controlled structures — PNG chunks, BMP
headers, CD sectors, gzip members, base64 runs — which is exactly where a crash would live.

Two tests added:

- `layered_streams_survive_mutation` builds a stream with `precomp = true` over a payload
  containing base64, a gzip member and binary noise, then mutates it 600 times.
- `every_unpacker_refuses_random_payloads` feeds 300 random buffers straight into
  `dedup_unpack`, `numeric_unpack`, `wav_unpack` and `gif_unpack`, and asserts each either
  refuses or produces something — never a silent empty success.

## Two bugs it found immediately

**1. `DecompressReport::original_size` lied for every transformed stream.** It reported the
header's `original_size`, which counts the *compressed-domain* payload, while `out` holds
what the inverse chain rebuilt. On a 2,684-byte stream with transforms on, the report said
2,684 and the output was 3,351 bytes. The field now reports the size the caller actually
gets. The bug was invisible to the old fuzz test because no transform ever ran in it.

**2. `dedup_unpack` accepted an empty encoding.** A payload declaring `total = 0` with no
commands returned `true` and produced nothing, so a caller could replace real data with an
empty buffer. Not exploitable — the block digest covers the payload before the inverse runs
— but it is the wrong answer to "is this a valid dedup stream", and it is now refused.

Both were found within a minute of the first run. That is the argument for fuzzing the
options the product actually uses, not the defaults.

## The encoder, fuzzed for the first time — and it held

The decoder has been fuzzed above and gave up two bugs in a minute. The **encoder** had
never been fuzzed at all, and it is the side that runs every transform's *detector* over
caller-supplied bytes: a PNG chunk walker, a BMP header parser, a CD sector scanner, a gzip
member finder. A malformed file must make each of them say "no" rather than crash, hang, or
accept something it then cannot reverse.

Two tests added, both asserting the stronger property — whatever the encoder decides, the
stream must decode back to **exactly the input bytes**:

- `hostile_containers_are_declined_or_round_trip`: a PNG with a chunk claiming 1 GiB it does
  not have, a BMP declaring 2^30 × 2^30 pixels in 1 KB of data, CD sectors with a mode byte
  of 0x7F, and a gzip member with no end.
- `random_bytes_never_crash_the_encoder`: 120 random buffers, every fourth one carrying a
  real PNG/BMP/gzip signature so the detectors are actually reached.

**No bugs.** Every case either declined the transform or round-tripped exactly. That is a
weaker result than the decoder fuzz gave, and it is worth saying which one it is: the
detectors were already defensive — they check lengths, bounds and magic before claiming
anything, and each transform verifies by rebuilding its input and byte-comparing before the
payload is returned. The encoder fuzz confirms that design rather than finding a hole in it.

The lesson from both runs is the same one, and it is about *coverage of the shipped
configuration*: the decoder fuzz found bugs the moment it stopped using default options, and
the encoder fuzz found none because the transform contract — rebuild and compare — is
already the check.
