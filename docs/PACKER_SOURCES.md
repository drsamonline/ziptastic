# Fifth batch: executable packers and build-time size reducers

A 23-item list of EXE packers and binary-size tools was supplied. The triage is
short because most of this list is a **category error** for a lossless archiver,
and naming the distinction is more useful than cataloguing the tools.

## The distinction

- **Compressors** take content and encode it smaller, reversibly. That is what
  ziptastic is, and what a container format gives us to exploit.
- **Build-time size reducers** take a program and *remove content* — symbols, debug
  info, unreachable functions, unused runtime classes. The output is a different,
  smaller program, not a compressed one. There is nothing to reverse and nothing to
  model.

Most of this list is the second kind, so it is out of scope by definition, not by
judgement.

## Out of scope: build-time size reducers (12 items)

`strip`, `llvm-strip`, `llvm-objcopy`, `ELFkickers` (sstrip), `Bloaty McBloatface`,
GCC, Clang, LLD, `mold`, `Binaryen wasm-opt`, `wasm-snip`, `Twiggy`, `ProGuard`,
`R8`, `.NET IL Linker`, `jlink`, `CompactGUI`.

These belong to a *build pipeline*, not an archive. `Bloaty` and `Twiggy` are
profilers — they find waste, they do not compress. `CompactGUI` drives NTFS
transparent compression, which is an operating-system feature, not an algorithm we
could adopt.

## Out of scope: historical reference

The DOS compressor history page (LZEXE, DIET, aPACK, 32LiTE) is explicitly a
reference page. Interesting, nothing to link.

## The one that is genuinely relevant: UPX

**UPX (GPL-2, cloned to `vendor/hcs/old/upx`)** is a real compressor with a real
format, and **UPX-packed executables are common inside archives** — which makes
them a container worth handling, exactly like ZIP, PDF, JPEG and GIF.

Two honest observations:

1. **We do not handle it.** `src/` contains no UPX awareness at all; the engine's
   only executable-specific transform is the x86 BCJ filter, which helps *inside* a
   binary rather than undoing a packer.
2. **It is GPL-2**, so it can only ever be a plugin under the rule already applied
   to bzip3 — and a plugin cannot help here, because the work is not "call UPX" but
   "recognise UPX's stub, decompress the payload, compress the original bytes, and
   reconstruct the packed file exactly". That is a *reimplementation* of UPX's
   container logic, which the GPL forbids us from copying and which is a
   substantial job to do from behaviour alone.

**Verdict: not worth it.** The population of UPX-packed files in a typical archive
is small, the reconstruction has to be byte-exact against a GPL implementation, and
the payoff is limited to that population. Contrast with LZ4, which is in a large
share of modern data.

## What this batch actually tells us

Nothing new about compression. It does reinforce the pattern from the previous four
batches: the lists arriving are organised by *"what is a good tool"*, and the
question that has produced every real win for this engine is *"what is a container
we cannot parse yet"*.

Of the five batches so far, the returns were: repack tools → **LZ4/ZSTD/LZO
precompression targets** (large); scientific/ML → **FSE reference + typed
decomposition** (moderate); compressors/PDF → **PNG re-filtering, JBIG2** (moderate);
audio/video → **OpenEXR ZIP mode** (small); packers → **nothing**.
