# Every transform, switched off — an ablation of the table in FINDINGS §2

2026-11-18. `FINDINGS.md` §2 states a number for each Layer 0 transform. Those numbers were
measured on the fixtures named there, over months, and none of them had been re-measured as a
set. This does that: one fixture per transform, built to match the row's description, run with
the transform on and off.

The switches did not all exist until today — `--no-jpeg` and `--no-gif` were added in #46 — so
this is the first time the whole table could be ablated without patching the source.

Method: `ziptastic -9` against `ziptastic -9 --no-<transform>`, same file, and the gain is how
much smaller the archive is with the transform on. Fixtures are synthetic and are **not** the
ones §2 used, so a different magnitude is expected; a **zero** is not, and that is what this is
looking for.

| §2 row | §2 claim | fixture here | measured | verdict |
|---|---|---|---|---|
| integer CSV columns | −77.4% | 40,000 rows, 3 ints, 547 KB | **99.7%** | fires; the fixture is more regular than a real CSV |
| base64 runs decoded | −19.5% | base64 of a 12 KB JPEG | **15.2%** | fires, close |
| gzip members inflated | −21.2% / −25.4% | `.xml.gz`, 103 KB → 958 KB XML | **0.0%** | **did not fire** |
| second container level | −44.8% | `.tar.gz` holding the CSV | **49.7%** | fires |
| WAV PCM per-channel delta | −16.4% (16-bit) | 2-channel 44.1 kHz sine | **74.3%** | fires; a pure sine is the best case for a delta |
| SQLite pages by B-tree type | −4.8% / −6.3% | 770 KB db, one table | **0.0%** | **did not fire** |
| BMP colour planes + row delta | −36.9% / −48.3% | 400×400 photographic BMP | **22.0%** | fires, smaller |
| GIF, top level | −44.6% / −48.8% | 400×400, 128-colour | **50.2%** | fires |
| PNG one level down | −0.3%, usually declines | ZIP of a photographic PNG | **0.0%** | declines, as documented |
| JPEG one level down | −24.9% | ZIP of a photographic JPEG | **12.9%** | fires, smaller |
| GIF one level down | −11.1% | ZIP of a photographic GIF | **50.1%** | fires, far bigger |
| BMP one level down | −36.9% / −48.3% | ZIP of a photographic BMP | **21.8%** | fires, smaller |
| base64 one level down | −19.7% | `.json.gz` holding base64 of a JPEG | **0.0%** | **did not fire** |

## The three that did not fire

**Two of them are one cause.** A `.json.gz` holding base64 keeps its base64 inside a gzip
member. If the member is not inflated, the base64 is never visible — so "base64 one level down"
depends on "gzip members inflated", and both read 0.0% here.

**The gzip row declines on a plain `.gz`.** Not a bug in the transform: `precomp_helps` compares
the inflated payload against the original, and for this file the engine's own compression of the
*`.gz` bytes* already reaches 5,453 bytes — the same as the transform would get from the XML. A
deflate stream of highly repetitive text is itself compressible, so inflating it buys nothing.
The same file as `.tar.gz` fires and is worth 49.7%, because there the payload is a container the
block loop cannot see into.

So the −21.2% in §2 is not wrong, it is **conditional**: it holds where inflating the member
exposes something the engine cannot already reach. On a `.gz` of text it does not, and the gate
correctly declines.

**The SQLite row did not fire on a one-table database.** §2 measured 295 KB and 3.46 MB databases;
this fixture is 770 KB with a single table and 30,000 rows. Grouping pages by B-tree type has
little to group when there is one B-tree, which is the likely reason. Unconfirmed — it needs a
multi-table database to say whether the row reproduces.

## What this does not settle

Every number above is from a synthetic fixture. The point is not that the magnitudes differ —
they should — but that **three rows produce nothing at all on a fixture built to match their own
description**, and two of those share a cause. That is worth knowing before anyone quotes §2 as
though each row were unconditional.

## The reporting trap, again

Twice while doing this the wrong field was read: the JPEG and GIF stages report in **`flags`**,
not `layers`, and the gzip stage reports in `layers` 0x20 while the PNG stage reports in
`layers2` 0x10. `ziptastic info` prints all of them and does not say which transform owns which
bit. Two of the "did not fire" conclusions above were nearly wrong for this reason, and were
caught only by comparing the archive against its `--no-` run rather than trusting the flag.

**That is the rule this document exists to state: the flag is a hint, the ablation is the
measurement.**
