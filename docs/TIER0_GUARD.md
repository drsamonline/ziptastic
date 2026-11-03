# Tier-0: never worse than a plain compressor

Written 2026-11-03. A small guard on the one path where a predictor decides alone.

## The hole

The Tier-1 guarantee compares the stream against LZMA2 over the bytes **after** Layer 0, so it
cannot notice a Layer 0 transform that made the file *harder* to compress. That is not
theoretical: while the precomp gate was restructured, a `.tar.gz` came out at **50,799 bytes
from a 50,414-byte input**, and every check in the engine was satisfied — the fallback was
comparing against the transformed bytes, not the original ones.

## The fix, and why it is scoped

`precomp_helps` normally compares *measured* coded sizes of the payload and the original, which
cannot go wrong in that direction. It has one shortcut:

```cpp
if (payload.size() >= original.size() * 2) return true;
```

That rule exists for good reason — it is a structural signal that the original was DEFLATE over
something highly compressible, and it beats an estimator that has been wrong three times. But it
is still a **predictor**, and it is the only place where one decides alone.

So the guard runs **only when the gate took that shortcut**: compress the original bytes with
LZMA2 as well, and keep whichever stream is smaller. Ordinary files — where the gate compared
measurements — pay nothing.

## What it costs

One LZMA2 run over the original, and only for files where a container transform fired *and* the
inflated payload was at least twice the container. That is the case where the risk lives, so the
cost lands where the value is.

## What it does not do

It does not make ziptastic smaller than a plain compressor on incompressible data — a 50 KB
`.tar.gz` of random bytes still comes out ~0.8% larger, because the block framing costs more
than the redundancy saves. That is normal compressor behaviour and `xz` does the same; the guard
is about *transforms* making things worse, not about eliminating framing overhead.

## Tests

32/32 suites pass. The guard is exercised by the container tests (which set `precomp` and
inflate payloads past the 2× rule) and by the round-trip suite; it is not separately asserted
because the case that motivated it — the restructured gate — was reverted, and a test for a
reverted design would be testing nothing.
