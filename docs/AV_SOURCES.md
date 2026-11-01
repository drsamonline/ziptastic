# Fourth batch: audio, video and image codecs — mostly out of scope

A 24-item list of audio, video and image codecs was supplied. This records the
triage, and the triage here is blunt because the answer is blunt.

## The filter that matters

**ziptastic is a lossless archiver.** A lossy codec is not a compressor we can
adopt — it is a *format* we might recompress. So each entry was judged on one
question only: **does it define a stream we could re-encode losslessly and
reproduce byte-exactly**, the way brunsli does for JPEG and our LZW work does for
GIF? Everything else is out of scope regardless of how good the codec is.

## Added (lossless-relevant, permissively licensed)

| repo | licence | why |
|---|---|---|
| **openexr** | BSD-3 | HDR images; its **ZIP mode is plain DEFLATE**, which preflate already handles |
| **WavPack** | BSD-3 | lossless audio |
| **charls** | BSD-3 | **JPEG-LS**, a genuine lossless image codec |
| **djvulibre** | **GPL** | scanned documents (JB2 + IW44) — documentation only, no code |

## The one concrete finding

**OpenEXR's ZIP/ZIPS compression modes are DEFLATE.** preflate already handles
DEFLATE; what we lack is a scanner that finds those blocks inside an EXR container
— exactly the shape of the PDF work, where the streams were zlib-wrapped DEFLATE
inside a `stream`/`endstream` frame. So an EXR file in ZIP mode is a *small,
well-understood* addition, not a new codec.

OpenEXR's other lossless mode, **PIZ**, is wavelet + Huffman. Recompressing PIZ
would mean re-implementing its encoder; not worth it for a format used in VFX
pipelines rather than typical archives.

## Not pursued, with reasons

**Lossy, therefore only interesting as a format — and none of these have a
practical lossless recompression path:**

Musepack, TwoLAME (MP2), Speex, Codec 2, LAME (MP3), Xvid, Theora, Daala, Dirac,
BPG, JPEG 2000 (OpenJPEG, JasPer), JPEG XL, FLIF.

Three deserve a note rather than a dismissal:

- **MP3** — recompression *is* real (packmp3), but it is **LGPL**, so it is
  plugin-only under the rule we already applied to bzip3. Already known.
- **FLIF** — genuinely lossless, but development has ended and it was superseded by
  JPEG XL; adopting an abandoned format is a liability, not an asset.
- **JPEG XL** — has a lossless mode and is the successor of both FLIF and JPEG, but
  recompressing a JXL stream means re-implementing its modular coder. That is a
  research project, not a plumbing job.

**Lossless but niche archival video/audio** — FFV1, HuffYUV, Snow, VC-2 (all inside
FFmpeg), Lagarith, Ut Video, Shorten, SLAC. These are already lossless-coded; the
gain from decoding and re-encoding them better is small, the formats are archival
rather than everyday, and FFmpeg alone is ~200 MB. Not cloned deliberately.

## Verdict

**This batch changes nothing in the plan.** Three permissively licensed repos are
now on the shelf for reference, and one of them (OpenEXR) yields a small, concrete
addition: an EXR container scanner in front of preflate.

The pattern is now unmistakable across four batches: **codec variety is exhausted
for us.** What keeps paying is *container coverage* — ZIP, PDF, JPEG, GIF each gave
19–79%, while every codec-level idea measured at 1–3% or lost outright. A list of
24 audio/video codecs contains no new container we do not already handle, and one
(EXR) that we can reach with existing machinery.
