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
| SQLite pages by B-tree type | −4.8% / −6.3% | see below — shape decides | **0.0% to 27.8%** | **settled: fires on multi-kind databases, and is worth 4× the claim** |
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

**The gzip row declines on a plain `.gz` — but only for *that* content.** Followed up on
2026-11-18 with four `.gz` files of different content, which is what the single fixture could not
tell us:

| `.gz` holding | bytes | transform on | `--no-precomp` | worth | fired |
|---|---|---|---|---|---|
| 547 KB of numeric CSV | 241,981 | **113,167** | 219,851 | **48.5%** | yes |
| a 12 KB JPEG | 12,123 | **10,616** | 12,227 | 13.2% | yes |
| a 116 KB PNG | 116,364 | **97,706** | 116,472 | 16.1% | yes |
| 2.5 MB of repetitive XML | 371,412 | 55,595 | 55,595 | **0.0%** | no |

So the transform is not weak — it is worth nearly half the file on a CSV — and it declines only
where it adds nothing: for the XML, the engine's own compression of the *`.gz` bytes* already
reaches 55,595, because a deflate stream of highly repetitive text is itself compressible. The gate
is right in both directions, and this document's earlier "did not fire" was one fixture standing in
for a content class.

So the −21.2% in §2 is not wrong, it is **conditional**: it holds where inflating the member
exposes something the engine cannot already reach. On a `.gz` of text it does not, and the gate
correctly declines.

**The SQLite row was unconfirmed when this document was first written, and is now settled —
the fixture's shape decides, exactly as `SQLITE_MEASUREMENT.md` implies.** The transform groups
pages by kind, so a database with one B-tree has nothing to group:

| database | pages | kinds present | archive | `--no-sqlite` | gain |
|---|---|---|---|---|---|
| one table, 30,000 rows | 188 | leaf-table only | 69,438 | 69,438 | **0.0%** |
| 4 tables + 8 indexes, 397 KB | 96 | all four | 22,548 | 22,548 | **0.0%** |
| 5 tables + 10 indexes, 6.9 MB | 1,685 | all four | **326,723** | 452,223 | **27.8%** |

So the row reproduces, and on a realistic indexed database it is **worth four times the −6.3%
§2 quotes** (27.8% against 6.3%). The small multi-kind database still declines, which is
consistent with §2's own smallest figure being only −4.8% at 295 KB: below a few hundred pages
the reordering does not pay for its permutation, and the gate is right to decline.

The honest summary: the transform is worth **nothing** on a one-kind database and **a quarter of
the file** on a large indexed one. §2's single number sits at the bottom of that range and should
not be read as the general case.

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
