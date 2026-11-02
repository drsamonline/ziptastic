# LZ4 precompression: measured, and the parse is not free

Written 2026-11-02. This is a **negative result**. `docs/BENCHMARKS.md` recorded
LZ4 as the highest-value unhandled container, at −28.8% to −58.2% on Silesia. Those
were *ideal* numbers — "compress the payload as if extracting it were free". It is
not free, and paying for it flips the sign.

## What was measured

LZ4 frames (`lz4.frame.compress`, level 1), parsed into their exact sequence list.
The packed form is what a real implementation must store:

```
plaintext | recipe | u32 recipe_len | u32 plain_len | "LZ41"
```

`recipe` holds the frame header verbatim, then per block its size, and per sequence
`varint(literal_len), varint(match_len), varint(offset)`. That is everything needed
to re-emit the original bytes exactly from the plaintext.

| fixture | raw | `.lz4` | sequences | recipe | engine on `.lz4` | engine on packed | result |
|---|---|---|---|---|---|---|---|
| spreadsheet XML | 3,250,684 | 980,883 | 292,391 | 1,056,411 | 121,343 | **181,709** | **+49.7% worse** |
| 4 MB of log lines | 4,021,362 | 968,383 | 236,281 | 979,978 | 119,019 | **179,641** | **+50.9% worse** |
| 1.65 MB of JSON rows | 1,650,000 | 6,884 | 52 | 274 | 289 | **209** | −27.7% |
| 400 KB of random bytes | 400,000 | 400,047 | 0 (stored) | 51 | 400,138 | 400,158 | −0.0% |

Same fixture, with the recipe zero-filled instead of written out — i.e. the
*ideal* case the earlier measurement described:

| fixture | engine on `.lz4` | ideal (free parse) | real |
|---|---|---|---|
| spreadsheet XML | 121,343 | 58,764 (−51.6%) | 181,709 (**+49.7%**) |

The ideal case was reproduced and then destroyed by the cost of storing the parse.

## Why it loses

The transform replaces "literals + LZ4's match descriptors" with "plaintext + our
match descriptors". A strong backend already codes LZ4's literals well and already
finds the redundancy the matched bytes represent — so the only thing that changes is
*whose descriptors are cheaper*. LZ4's own encoding is 1–2 bytes per sequence; ours
is three varints, and on this data they are barely compressible because they are
essentially random.

Trimming the recipe does not rescue it. Match lengths are derivable from the next
literal position, so only `(literal_len, offset)` is strictly needed — measured at
**72–76% of the full recipe**, still 744 KB against a 968 KB LZ4 file.

The pattern is exact: the transform pays only when LZ4 found *few, long* matches,
because that is when the descriptor stream is small. That is also when LZ4 already
did most of the work, which is why the win is small (27.7% on the JSON fixture,
which had 52 sequences for 1.65 MB) and never appears on data where LZ4 was
actually earning its speed.

## Why this differs from preflate

Preflate works because a DEFLATE parse is very nearly recoverable: the correction it
stores is a few bytes per stream, not per sequence. The earlier note claimed LZ4
would be easier than GIF because "the parse can simply be stored and re-emitted".
Storing is exact, but it is not cheap — and cheap was the whole question. Exactness
was never the risk here; the ratio was.

## What would change the answer

**Re-encode and compare.** Store only the compression parameters, run an LZ4 encoder
on the plaintext, and accept only when the output matches byte-for-byte. Then the
recipe costs nothing and the full ideal gain (−51.6% on the spreadsheet fixture) is
real. The catch is that it needs a bit-exact reproduction of liblz4's greedy matcher
— its hash table size, skip heuristic and last-five-literals rule — and a mismatch
means declining the file. That is the GIF problem again, with the difference that
LZ4's encoder is at least deterministic for a given version and level.

That is worth trying before this item is written off. What is **not** worth doing is
shipping the store-the-parse version behind a trial: it would fire only on the rare
file where LZ4 left few matches, and on everything else it would cost the trial and
return nothing.

## Status

Not implemented. Bit 5 in `format.hpp` stays free. If the re-encode route is taken,
this document's table is the baseline it has to beat.
