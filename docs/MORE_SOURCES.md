# Third source batch: compressors, PDF and image tools

A 30-item list of compressors, PDF processors and image optimisers was supplied.
This records what was already here, what was materialised, what was added, and
what is a *tool* rather than an algorithm — the distinction that matters, because
a GUI wrapper teaches us nothing we can link.

## Already on the shelf (no action)

`openzl`, `zpaq`, `libbsc`, `zopfli`, `precomp-cpp`.

## Were registered submodules but empty — now materialised

| repo | licence | note |
|---|---|---|
| **lzham** | **public domain** | high-ratio LZ; the cleanest licence in the whole batch |
| **lizard** | BSD / GPL dual | use the BSD side |
| **snappy** | BSD (Google) | speed-oriented LZ |
| **heatshrink** | ISC | embedded, tiny |
| **brieflz** | zlib | tiny LZ |

## Newly added

| repo | licence | what it gives us |
|---|---|---|
| **kanzi** | **Apache-2.0** | a catalogue of *transforms* (see below) |
| **lzfse** | BSD (Apple) | LZ + **FSE** — a second reference for catalogue item #2 |
| **jbig2enc** | Apache (Google) | **JBIG2** for scanned black-and-white pages |

### kanzi's transforms are the useful part

`java/.../transform/`: **BWT**, **BWTS** (bijective BWT, no primary index),
**SRT/SBRT** (sorted rank transform), **ROLZ**, **LZPX**, **RLT**, **ZRLT**,
**EXECodec** (x86), **TextCodec**, **UTFCodec**, plus DivSufSort and SA_IS.

Apache-2.0 and readable. Two are genuinely interesting for us:

- **SBRT / SRT** — a BWT-family transform that is *faster* than BWT to compute and
  often close in ratio. We currently pay full suffix-sort cost for libbsc's BWT;
  SBRT is a cheaper alternative worth measuring.
- **TextCodec / UTFCodec** — text-specific transforms, a different angle on the
  word-dictionary idea that measured −0.38% on English and +0.93% on Polish.

## Tools, not algorithms — deliberately not pursued

`qpdf`, `pdfcpu`, `ouch`, `pikepdf`, `Ghostscript`, `minimalpdfcompress`,
`DeveMultiCompressor`, `ECT`, `rapidgzip`, `pixz`, `plzip`, `crabz`, `pdfsizeopt`,
`pngquant` (lossy), `jpegoptim` (often lossy).

They are useful programs but they either wrap codecs we already have, are lossy, or
are format processors with no reusable engine component. Listing them here so the
next scan does not re-open them.

## The two that are more than tools

**oxipng — PNG re-filtering.** PNG is DEFLATE, which we already handle through
preflate, so PNG *content* is covered. What we do not do is re-choose the per-
scanline filter (None/Sub/Up/Average/Paeth). A PNG written with a poor filter
choice compresses worse than one written with a good one, and the filters are
reversible — so re-filtering is a lossless transform that changes the DEFLATE input
before preflate sees it. **Implementable in the engine**, small, and it stacks with
what we already do.

**jbig2enc — JBIG2 for scanned pages.** Scanned PDFs hold bilevel images coded with
CCITT G4 or JBIG2. JBIG2 with symbol dictionaries is dramatically better than G4 on
text pages, and a re-encode that reproduces the original bytes is the same shape as
our GIF work. **Implementable in the engine**, larger job, and the win is large
specifically on scanned documents — a very common real-world PDF type.

## Verdict

- **Engine, next after LZ4**: oxipng-style PNG re-filtering (small, lossless,
  stacks with preflate).
- **Engine, later**: JBIG2 for scanned PDFs (large win, large job).
- **Measure when convenient**: kanzi's SBRT as a cheaper BWT alternative.
- **Nothing else here changes the plan**; the remaining value in this list is in
  *formats*, not codecs, which is consistent with every earlier finding.
