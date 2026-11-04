# ziptastic

[![Licence](docs/assets/badge-licence.svg)](LICENSE)
[![Dependencies](docs/assets/badge-deps.svg)](docs/BUILD.md)
[![Build](docs/assets/badge-build.svg)](docs/BUILD.md)
[![Live](docs/assets/badge-live.svg)](https://drsamonline.github.io/ziptastic/)

**One self-contained binary that compresses a game asset tree 20–36% smaller than `xz -9`, and
tells you when it cannot.** No external processes, no network, no model files.

```bash
ziptastic c assets/          # → assets.dr
ziptastic d assets.dr        # → byte-identical
ziptastic info assets.dr     # → header, block table, offsets
```

`.dr` is the format's extension. Detection is by the `CHCE` magic, not the name: a `.dr` renamed
to `.bin` still decodes, and a file that is not a CHCE stream is refused.

## What it is actually good at

Format-aware transforms under a strong backend. A ZIP of PNGs, a raw CD image, a spreadsheet, a
packed asset tree — the engine recognises the container and the payload, undoes the compression
that is already there, and re-compresses what is underneath.

| case | ziptastic `-9` | `xz -9e` | |
|---|---|---|---|
| Mixed game-asset tree, 6.3 MB, as `.tar` | **1,275,048** | 1,983,636 | **−35.7%** |
| The same tree as a `.zip` | **1,284,836** | 1,610,228 | **−20.2%** |
| 6.8 MB spreadsheet sheet XML | **70,090** | 163,656 | −57.2% |
| 4.7 MB raw CD image (Mode 2, with ECC) | **1,417,452** | 2,014,224 | −29.6% |
| 2 MB English prose | **408,421** | 543,572 | −24.9% |
| 4.8 MB cooked ISO | 3,333,904 | 3,339,112 | −0.2% |
| 1.4 MB floppy image | 1,257,830 | 1,257,792 | +0.0% |

Every row round-trips byte-exactly. Full tables, the audit and the rejected ideas:
**[`docs/FINDINGS.md`](docs/FINDINGS.md)**.

## What it is not

- **Fast.** 0.3–0.6 MB/s at `-9` against `xz`'s 1–2 MB/s on the same machine. The ratio is bought
  with time, deliberately. `--parallel` trades ratio for speed.
- **A general-purpose winner.** Where no format transform fires it is level with `xz`, and twice
  marginally behind. The advantage is concentrated in a handful of formats, not spread evenly.
- **Magic on incompressible data.** A cooked ISO gains 0.2%; a file of random bytes gains nothing.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # 34 suites, ~4 minutes
```

Linux and Windows. macOS is not supported. The long form — vendor-by-vendor builds, the zig
toolchain, and the depfile trap that costs a debugging session if you hit it — is in
**[`docs/BUILD.md`](docs/BUILD.md)**.

## What is inside

Format **v8**. Per block, nine encoders compete and the smallest wins: RAW, rANS (static and
adaptive), range, static Huffman, BWT (libbsc), PPMd, an LZMA2 fallback that guarantees the output
is never worse than `xz`, and context mixing under `--max-ratio`. Above them sit eighteen Layer 0
transforms — container inflation (ZIP, gzip, PDF), base64, spreadsheet cells, CSV columns, WAV PCM,
SQLite pages, CD-sector Reed-Solomon parity, and PNG/JPEG/GIF/BMP one level down inside archives.

Each block carries its own SHA-256, the stream carries a manifest digest, and a `true_size` field
makes the decoded length an exact check rather than a heuristic. The trailer also carries a
**block offset table**, so a block can be reached without walking the stream: `ziptastic info`
prints it, `ziptastic d --block N` decodes one block on its own, and `--from-block N` resumes a
decode that stopped. Reading the index touches under 1% of an archive.

## Where to read next

| | |
|---|---|
| [`docs/FINDINGS.md`](docs/FINDINGS.md) | every measured number, the audit, what was rejected, what is open |
| [`docs/RELEASE_PLAN.md`](docs/RELEASE_PLAN.md) | 1.0 first, speed after, and why in that order |
| [`docs/BUILD.md`](docs/BUILD.md) | building it, including the traps |
| [`docs/FORMAT.md`](docs/FORMAT.md) | the stream format |
| [`docs/index.html`](docs/index.html) | the landing page |

## Support

| | |
|---|---|
| UPI (India) | `drsohilboi@ybl` |
| Ko-fi | [ko-fi.com/matneo](https://ko-fi.com/matneo) |
| PayPal | [paypal.me/drsohilmomin](https://paypal.me/drsohilmomin?locale.x=en_GB&country.x=IN) |
| Books | [Amazon author page](https://www.amazon.com/stores/author/B08ZT331LY/allbooks?ccs_id=aed72b9f-71d9-41dc-adee-e9368a317acc) |
| Everything else | [drsamonline.github.io](https://drsamonline.github.io/) |

## Licence

MIT for ziptastic itself. It links vendored compressors — liblzma, brotli, brunsli, libbsc, zpaq,
ppmd, preflate — whose terms are summarised in [`docs/LICENCES.md`](docs/LICENCES.md), with
`vendor/hcs/SOURCES` as the machine-readable authority. If you ship a statically linked binary,
read the LGPL paragraph there: it is the one that asks something of you.
