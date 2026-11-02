# Split archives: measure first, and the answer is "reassemble"

Written 2026-11-02. Item 6c was "split archives — `.z01/.z02/…/.zip` multi-volume ZIPs
and `.001/.002` byte splits". Measured before building anything, as the handoff asked.

## Result: compressing parts separately costs about 2x

| what | bytes | compressed |
|---|---|---|
| `data.tar` whole | 1,802,240 | **10,263** |
| `data.tar.001` + `.002` separately | 901,120 + 901,120 | **20,857** (2.03x worse) |
| `data.zip` whole | 225,318 | **10,445** |
| `data.zip` split in two, parts separately | 112,659 + 112,659 | **30,425** (2.91x worse) |
| both, concatenated back and compressed | as whole | 10,263 / 10,445 |

Concatenating the parts reproduces the original byte for byte in both cases (verified
with `cmp`), and the whole-file gain comes straight back. The loss is not a transform
that failed — it is the redundancy *between* parts, which no single-file compressor
can see.

## Why this is not an engine item

ziptastic takes one file. A split archive is several by definition, and the parts share
structure that only exists across the boundary:

- a `.001/.002` byte split cuts anywhere, including through a DEFLATE stream, so
  precomp can inflate neither half;
- a multi-volume ZIP keeps its central directory in the last part and its local headers
  spread across the others, so a part is not a container at all — it is a fragment of
  one.

The engine already does the right thing with a lone fragment: it compresses it exactly
(no crash, no corruption, round-trip exact) and simply gets the ~2x worse result the
table shows. There is no Layer 0 transform that can fix that from inside one part.

## Where it belongs

**heavenzip.** It already spawns processes and drives whole pipelines, so it can:

1. detect a split set (`.z01`/`.z02`/…/`.zip`, or `.001`/`.002`, or a `.partN` pattern);
2. concatenate or join volumes into the single logical file;
3. hand *that* to ziptastic (or to srep/7z/arc in the other pipelines);
4. re-split the output to the same part sizes, or record the original part boundaries
   so the archive can be restored exactly.

Step 4 is the only part with an exactness question, and it is trivial: store the part
sizes, which is a handful of integers.

## What is left for ziptastic

Nothing, unless someone wants per-part output — which would mean the engine writing a
split archive, a feature nobody has asked for and one that belongs with the archive
writer rather than the codec.

`SESSION_HANDOFF.md` §6c is marked measured, with these numbers.
