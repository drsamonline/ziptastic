# What arrived on 2026-11-07, and what any of it is worth

Thirty-seven distinct archives landed on 2026-11-07: a phone share, a repacking kit, and a set of
Windows compression and packaging tools. They were extracted and read. This is the verdict for each,
written down because the next person to be handed a folder like this should not have to open it.

**The short version: there is no compressor implementation here except one, and it is GPL.** The rest
are Windows GUI shells around tools that already exist, or installers that ship a `system32` tree.

## What is usable, and what for

| source | what it gives | licence | kept? |
|---|---|---|---|
| **FreeArc 0.67** | `arc.ini`: a real repack catalogue with per-type branches; a benchmark report with ratios for ARC, RAR, lpaq and durilca | Freeware | `vendor/hcs/old/freearc/` |
| **SREP 3.9** | the huge-dictionary LZ77 preprocessor's own documentation: dictionary larger than RAM, memory ∝ 1/L | Freeware | `vendor/hcs/old/srep/` |
| **FileOptimizer** | a plugin inventory of every format with a known optimiser — 55 tools | Freeware | inventory only, below |
| **Lprepaq 1.3** | `lprepaq.cpp`, 1,693 lines: a PAQ-family CM compressor wrapped around `precomp.dll` and `packJPG.dll` | **GPL-2** | **no** — read, not vendored |

Everything else — 7 UPX GUIs, 11 zip and archiver GUIs, `7zSFX_Constructor`, `7zbatch`,
`AppCompactor`, `Arc_Convert`, `Arc_Them_ALL`, `CollectivelyComp`, `UA`, `MC`, `MaskedCompression3`,
`Black.Box.Script`, `UniExtract`, `KGB` — is Windows binaries with no source and no licence that
permits vendoring. None of it can run here, so none of it was measured, and none of it is kept.

### Why Lprepaq is read and not vendored

It is the closest thing in the whole drop to what this project is: a pipeline that runs
precompression first and a context-mixing coder second, which is the shape `REPACK_PIPELINES.md`
argues for and the shape ziptastic arrived at independently. But it is GPL-2, and this repository is
MIT with statically linked vendored sources. Reading it for its parameter choices is fine; putting it
in the tree is not.

What it confirms without needing its code: `lprepaq N input output`, N from 0 to 9, memory
`6 + 3·2^N` MB — so 9 MB at the low end and 1,542 MB at the top, which is the same memory-for-ratio
trade this engine's `--max-ratio` makes, and a useful sanity check that the trade is normal rather
than excessive.

### The FreeArc catalogue is the most useful artefact here

`arc.ini` states, in the tool's own configuration, what a mature archiver does with different kinds
of input:

```
maxprecomp = precomp:intense
maxsrep    = srep:m3f:mem256mb
#p = maxprecomp + exe2 + maxsrep + #xb / $text=#t / $wav=#$wav / $bmp=#$bmp
#n = #bn / $text=#tn / $obj=#xb / $compressed=rep:64m + slug / $wav=tta / $bmp=mm+blizs
```

Read it as a design statement: precompression and dedup first, then a per-file-type branch — text gets
a PPMd-shaped chain, audio gets TTA, BMP gets a colour-plane transform, already-compressed data gets
a small-window dedup and nothing else. That is the same reasoning as this engine's Layer 0 selection,
with the difference that FreeArc dispatches on the *file name* and ziptastic dispatches on *content*.

The benchmark report is the only competitor table in the drop with real numbers on real data — a
Visual C++ source tree, 1,876 files, 41.8 MB — and it puts `-mlpaq` at 0.101 of the original against
`-m6` at 0.538, which is the ratio a CM coder buys and what it costs.

### The FileOptimizer inventory

Fifty-five format-specific optimisers, each one a format where somebody found a win. Worth keeping as
a list of candidates rather than as files:

```
7z DeflOpt ECT Leanify PETrim PngOptimizer TruePNG advdef advmng advpng advzip apngopt bestcfbf
cpdfsqueeze csstidy cwebp defluff docprc dwebp ffmpeg flac flacout flasm flexiGIF gifsicle
gswin64c guetzli gzip imagew jhead jpeg-recompress jpegoptim jpegtran jsmin m7zRepacker magick
mkclean mozjpegtran mp3packer mp4file mutool optipng pingo pngout pngquant pngrewrite pngwolf
rehuff rehuff_theora shntool sqlite3 strip tidy upx zRecompress
```

**bzip2 is on that list and is now measured**: a `.tar.bz2` costs the engine 9.7% more than the tar
does, and its level is in its own header (`BZh9…`), so the recipe would be one byte — but `libbz2` is
not vendored and `.tar.bz2` is rarer than `.tar.xz`, so it is recorded and not built
(`FINDINGS.md` §3h).

Four of these are formats ziptastic already transforms (PNG, JPEG, GIF, BMP), and two more on the
list turned out to be covered after all: **PDF** is reached by `precomp`, and a **SWF**'s zlib body
compresses better through the backend than its own decoded content does, so neither needs a transform
on the evidence measured (`FINDINGS.md` §3h). MP3 and FLAC remain genuinely untested. Three are formats it
does not and that have a known optimiser: **MP3** (`mp3packer`), **FLAC** (`flac`), **PDF**
(`cpdfsqueeze`, `mutool`), plus **CSS/HTML** (`csstidy`, `tidy`) and **SQLite** (`sqlite3`, which this
engine does handle as a page reordering). That is a candidate list with prior evidence behind it,
which is what Phase 4 asks for before anything is built.

## Two files that are not what their names say

- **`PRECOMP.7z` is 90 bytes** — a 7z header containing one empty directory, `PRECOMP/`. Nothing to
  extract. Either a truncated upload or a shortcut that was archived instead of the folder it points
  at.
- **`Batch.7z` contains twelve `.dr` files** which are PE32+ Windows executables. The extension
  coincides with this project's archive extension and means nothing here.

`KGB_Archiver_2beta2.7z` deserves its own line: it is an installer dump, and most of it is a copy of
`Windows/system32` and `winsxs`. It is not a tool you can look at.

## How these were opened

`py7zr` cannot read **BCJ2**, which every archive in this batch uses, so the extraction needed a real
decoder: `izextract`, built from the 7-Zip C sources this repository already vendors
(`vendor/hcs/old/7z/C`). That is a survey tool, not part of ziptastic, and it is not in the tree.
