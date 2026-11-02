# "zip store" does not map onto ziptastic — measured

Written 2026-11-02. `REPACK_PIPELINES.md` recorded the hand pipeline
`zip store -> precomp -> srep -> 7z|arc` and proposed stored-ZIP unpacking as the
first thing to build. Measured it. **It is not worth building**, and the reason is
structural rather than a matter of tuning.

## What was measured

A game-like tree: 700 KB of text, 1.3 MB across three near-duplicate data files,
300 KB of random binary, plus three nested containers (an `.xlsx`, a PNG and a
`.gz`). Packed two ways and compared.

| container | size | engine on it | `flags`/`layers` |
|---|---|---|---|
| `zip -0` (store) | 2,447,516 | **374,087** | 0x21 / 0x00 |
| `zip -9` (deflate) | 659,293 | 374,235 | 0x25 / 0x00 |
| members concatenated, no container | 2,446,738 | 379,460 | 0x01 / 0x00 |
| sum of members compressed one by one | — | 404,076 | — |

- **The store-zip already compresses best.** 374,087, against 379,460 for the same
  bytes with the container stripped off and 404,076 for the members compressed
  individually. Unpacking would make the result *worse*, not better: the zip's local
  headers and central directory are highly repetitive, and the engine collects that
  redundancy for free.
- **Per-member compression loses badly** (404,076 against 374,087) because the three
  near-duplicate `level*.dat` files cost 19,171 each on their own and are nearly free
  together. That is exactly what the dedup stage is for, and it only works because
  the container is one stream.
- **precomp does fire on a deflated zip**, contrary to the note in the pipeline doc:
  `flags` 0x25 shows `kFlagPrecomp`, and it is worth 398,964 → 374,235 (−6.2%). The
  earlier impression that it "does nothing" came from the store case, where there is
  by construction nothing to inflate.

## The nested-container case, isolated

The only argument left for unpacking was that it would expose containers nested
inside a store-zip to the transforms. Isolated it:

| | size | engine |
|---|---|---|
| `zip -0` of {xlsx, png, gz} | 150,269 | 19,242 |
| the same three compressed individually | — | 18,716 |
| `zip -9` of the same three | 23,889 | 19,364 |

**526 bytes, 2.7%** — on a container that is 99% one nested container, which is the
most favourable case that exists. On a realistic tree the nested files are a small
fraction and the number goes to noise.

## Why the pipeline step does not carry over

`zip store` exists in the repack workflow because the tools downstream are
**file-oriented**: precomp, srep and 7z each want to see a file's bytes, so the
repacker first removes the container's compression to stop it hiding anything.
ziptastic never had that problem — it takes the whole container as one stream and
compresses it directly, with dedup and per-block backend selection working across
the entire thing.

So the first step of the hand pipeline is already implicit here, and the measurement
says making it explicit costs 1.4% on the fixture rather than saving anything.

## What this leaves

- **Stored-ZIP unpacking: dropped** from the roadmap. `SESSION_HANDOFF.md` §6a-bis is
  marked as measured and rejected, with these numbers.
- **precomp on deflated zips: confirmed working** at −6.2% on this fixture. Worth
  knowing, because the field impression was that it often does nothing.
- **The remaining gap is still tar-inside-gzip.** `book.tar.gz` gains 30% today; the
  ideal 89% needs the tar parsed *and* the ZIP inside it reached, which means
  precomp has to scan for local headers anywhere in the payload rather than requiring
  offset 0 — the same change SFX needs. That is item 6a + 6b together, and it is
  where the next session should go.

## Reproducing

```bash
mkdir tree && ...                                   # any mixed tree
(cd tree && zip -0 -r ../store.zip .)               # the "zip store" step
ziptastic c -9 -o store.zt store.zip                # 374,087 on this fixture
cat tree/* > concat.bin && ziptastic c -9 -o c.zt concat.bin   # 379,460
```
