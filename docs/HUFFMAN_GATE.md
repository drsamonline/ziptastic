> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# Adaptive Huffman: 22% of the run, never a byte smaller

Written 2026-11-02, the concrete next step from `SELECTOR_PROFILE.md`.

## The measurement

The profile said "Huffman costs 18% and wins nothing". Splitting the candidate into its
two halves showed which half:

| configuration | 1 MB prose | size |
|---|---|---|
| default (both Huffman candidates) | 3,867 ms | 206,324 |
| `--no-huff-static` | 3,769 ms | 206,324 |
| `--no-huff-adapt` | 3,009 ms | 206,324 |
| `--no-huffman` | 2,779 ms | 206,324 |

**Static Huffman costs 98 ms (2.5%). Adaptive Huffman costs 858 ms (22%).** Neither
changed a single byte of the output on that file.

So the question became: does the adaptive candidate ever win anywhere? Four inputs, one
per shape Huffman is supposed to help with:

| input | default | `--no-huffman` | adaptive's contribution |
|---|---|---|---|
| English prose, 1 MB | 206,324 | 206,324 | none |
| 90% zero bytes, 400 KB | 65,332 | 65,332 | none |
| seven-symbol alphabet, 400 KB | 93,271 | 94,120 | none — **static** wins the 0.91% |
| low-entropy structured binary, 400 KB | 267 | 267 | none |

The only case where Huffman won at all was the seven-symbol file, and the win came from
the **static** candidate: `--no-huff-adapt` produced the identical 93,271 bytes. The
adaptive candidate has not produced a smaller stream in any test run here.

## What shipped

`huff_adaptive` is **off by default**. Same output on all four inputs, and:

| input | default now | with `--huff-adapt` | saving |
|---|---|---|---|
| 1 MB prose | 3,180 ms | 4,442 ms | **28%** |
| seven-symbol, 400 KB | 1,763 ms | 2,500 ms | 29% |
| 90% zero, 400 KB | 1,791 ms | 3,021 ms | 41% |
| low-entropy binary, 400 KB | 2,446 ms | 2,700 ms | 9% |

The code is untouched and `--huff-adapt` turns it back on, so a future input that wants
it can still have it — but it no longer pays on every block by default.

This is a *measurement*, not a gate: four inputs, four shapes, no win. A sample-based
trial would have been the alternative, and it would have been a prediction with a margin
this thin — the honest reading is that the candidate is not earning its place.

## Two new flags, and why they exist

`--no-huff-static` and `--huff-adapt` (and the existing `--no-huffman`) are there so
this can be re-measured. The first attempt at this profile was wrong twice — see
`SELECTOR_PROFILE.md` — and both mistakes would have been caught by running the two
candidates separately from the start.

## A trap that cost a build cycle

Adding two fields to `CompressOptions` changes its layout. The sandbox build has
depfile tracking stripped (see `INTEGRITY.md`), so the test binaries were **not**
recompiled against the new header and read `parallel` from the wrong offset — two tests
failed with "parallel flag not set" and looked like a logic bug. Touching every source
and rebuilding fixed it. Any change to a struct that crosses the library boundary needs
a full rebuild here, not an incremental one.
