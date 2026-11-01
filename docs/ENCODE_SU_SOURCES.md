# Sixth batch: encode.su and the demoscene download area

Uploaded packages plus a crawl of encode.su and thedubber.altervista.org.

## Uploaded and shelved

| package | licence | what it is | usable? |
|---|---|---|---|
| **all-uc2.zip** | **LGPL-3** | **UC2** (Bulat Ziganshin) with full source | reference only — plugin at best |
| picocrush.zip | none | `crush32`/`crush64` binaries | binaries only |
| crush_v100_3686_bytes.7z | none | `crush.exe` binary | binaries only |
| miniCRUSH.7z | none | `miniCRUSH.exe` binary | binaries only |

### UC2 is the real find here

177 source files including two substantial context-mixing implementations:

- **`SUPERMAN.CPP`** — 2,909 lines
- **`COMOTERP.CPP`** — 2,700 lines (COntext MOdelling TErtaint Predictor)

This is a *complete, working* CM compressor from the author whose work this project
has already studied. It is **LGPL-3**, so it cannot be linked into the MIT core —
but as a **reference for the CM backend** (catalogue item #1) it is far more
concrete than a paper: it shows the actual arrangement of models, counters and
mixing that achieves the numbers.

It is also a useful reality check. UC2 is a *pure* CM compressor and it is not
faster than what we have; its interest is ratio, and its models are exactly the
kind of thing that would go behind a plugin boundary.

**The 7z packages could not be extracted** — both use the BCJ2 filter, which py7zr
does not support. They contain single Windows executables of CRUSH (Ilya Muravyov's
tiny CM compressor) with no source, so nothing is lost.

## Crawl results

**Valuable — a published reference.**
`encode.su/threads/4134` points to ***Text Compression*** (1990), Bell, Cleary &
Witten, free at compressme.net. This is the foundational text on PPM and arithmetic
coding — exactly the material the CM backend needs, from a legitimate published
source. Worth downloading and working from.

**Valuable — a better test corpus.**
`encode.su/threads/4416` preserves the **Maximumcompression benchmark files**
(14.3 MB): `A10.jpg`, `acrord32.exe`, `english_mc.dic`, `FlashMX.pdf`, `fp.log`,
`mso97.dll`, `ohs.doc`, `rafale.bmp`, `vcfiu.hlp`, `world95.txt`.

**This matters more than any codec in the batch.** That set contains a **JPEG, a
PDF, a DOC, an HLP, a DLL and a BMP** — precisely the containers our Layer 0 work
targets. Silesia, our current corpus, contains **none** of them, which is why every
container win this project has measured (−19% JPEG, −79% PDF, −29% GIF, −77% CSV)
had to be measured on separately generated files rather than on the standing
corpus. Adopting this set would let the headline number reflect the work.

**Not useful.**
- `threads/2460` (miniCRUSH) and `threads/3020` (smallest LZMA decompressor) are
  both about **executable size**, not compression ratio. The second mentions
  LzmaFlow, a refactored LZMA *decoder* from UPX — decode *size*, not speed.
- `thedubber.altervista.org/download/nexopacker/` — a Windows SFX creator built on
  **raw Deflate**. We already handle Deflate, and the tool is Windows-only.
- `forums/4-Download-Area` and `members/8-Bulat-Ziganshin` are index pages; the
  attachments behind them need a forum login.

## What this changes

1. **A reference implementation for the CM backend** (UC2), concrete rather than
   theoretical, behind the plugin boundary.
2. **A corpus that matches the work.** The standing −11.83% is measured on Silesia,
   which contains no containers at all — so it systematically understates what this
   engine does to real-world files. The maximumcompression set is the right
   complement and should be added as a second corpus.
3. Nothing here changes the plan otherwise: no new codec, and the same conclusion
   as the previous five batches — **containers pay, codecs do not.**
