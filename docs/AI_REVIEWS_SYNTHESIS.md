# Nine AI reviews, synthesised against our measurements

Nine models answered the brief. This records what they converged on, what is new,
what is already disproved, and **two corrections to my own plan that I accept**.

## The corrections I accept

### 1. ECM is narrower than I claimed — demote it

Two models independently caught this, and they are right:

> "The ~12% applies only to raw 2352-byte-sector images (BIN/NRG/CDI); cooked ISOs at
> 2048 bytes/sector contain no EDC/ECC and see zero reduction."

A standard `.iso` is **cooked** — 2048-byte sectors, no sync, no EDC, no ECC. The
ECM gain exists only on **raw** CD rips (`.bin`, `.nrg`, `.cdi`). I had been quoting
"~12% on CD images" without that qualifier, which overstates it: the common case in
a real archive is an `.iso` with nothing to strip.

ECM remains worth doing — the transform is small and the spec is public — but it is
**not** the top of the queue, and I was wrong to put it there.

### 2. PAQ-class ratio at 3–8 MB/s is not realistic

> "Nobody has published a CPU CM design at 5 MB/s. LPAQ — deliberately lite — runs
> ~0.45 MB/s. cmix takes hours."

That matches our own calibration: my BWT+CM attempt was slow *and* lost 4.5–11.3%.
The honest conclusion is that a CM backend buys single-digit ratio at a
multiple-of-xz slowness. It stays last, and I should stop describing it as a likely
win.

## The strongest new idea: recursion, not more formats

Two models led with this, and it is the best idea in the whole set:

> "The biggest gap isn't another container — it's nesting. Real archives are nested
> two or three deep: .tar containing .docx/.xlsx/.apk/.jar (all ZIP containers of
> XML)."

The killer example: **an XLSX is a ZIP whose worksheets are XML full of numbers.**
Our numeric transform measures −77.4% on integer columns, and it is currently
**idle** on exactly the data it was built for, because the numbers are inside a ZIP
inside an XLSX and our Layer 0 chain runs once.

The fix is small and fits the existing architecture: after any transform produces
output, re-run detection on the result, depth-limited. Our transforms already
produce bytes; nothing needs redesigning, only re-entering.

**This is now the top item.** It multiplies every transform we already have, and it
is the only suggestion that makes existing work pay more without new format support.

## New formats worth adding, in the order they were argued

| format | argument | exactness |
|---|---|---|
| **Base64 / hex / quoted-printable** | ubiquitous in .eml, MIME, JSON, XML, source trees; 33% overhead removed for free | trivial — canonical base64 round-trips |
| **WAV / AIFF PCM** | "biggest single available win, ~45–60%"; per-channel delta, *not* shuffle (already measured failing) | header + fixed stride |
| **SQLite** | 4 KiB pages, page-type byte, cell-count, freeblock/freelist pages near-zero; common in backups, pathological for general compressors | public file-format spec |
| **OOXML (docx/xlsx/pptx)** | falls out of recursion + the numeric transform | ZIP + XML |
| **PCAP/PCAPng** | per-record timestamps, length fields duplicating data | public spec |
| **ELF/PE/Mach-O + DWARF** | relocation tables, symbol string tables, alignment padding | public specs |
| **zstd / brotli frames** | RFC 8878 / 7932; now common in .deb, .rpm, OCI layers | same preflate-style correction |
| **BGZF/BAM** | multi-member gzip = DEFLATE blocks; nearly free once multi-member gzip is handled | preflate already covers the streams |
| **TIFF, FITS, FASTQ, DICOM, Parquet** | real but narrower populations | public specs |

## Cheap wins nobody should argue with

**Parallelise the try-everything selector.** Our transforms are independent; we run
their attempts serially. Running them on all cores and keeping the winner costs no
ratio and cuts encode time by roughly the core count. Two models flagged it, and it
is the cheapest change on this list.

## Already disproved — raised anyway, and correctly declined

Two models explicitly noted they had read the failure table and avoided it ("not
shuffle — you already measured shuffle fail"). One proposed bitshuffle for numeric
data despite the brief; that remains refuted (+4.8% to +27.6%), and the *typed*
per-column version is what we already ship.

## Revised order

| # | item | why | effort |
|---|---|---|---|
| 1 | **Recursive container descent** | multiplies every existing transform; unlocks XLSX/APK/JAR | small |
| 2 | **Base64/hex decode** | ubiquitous, exact, trivially reversible | small |
| 3 | **Parallelise the selector** | free speed, no ratio cost | small |
| 4 | **WAV/PCM per-channel delta** | ~45–60% claimed on a common format | small |
| 5 | **SQLite pages** | common, public spec, pathological for LZ | medium |
| 6 | **LZ4** | −28.8% to −58.2% measured | medium |
| 7 | zstd/brotli frames, PCAP, ELF/DWARF, TIFF | real, narrower | medium |
| 8 | ECM (raw images only) | ~12%, narrow population | small |
| 9 | CM backend | single digits, multiple-of-xz slow | large |

Items 1–4 are all small, and three of them multiply work already done rather than
adding new formats. That is a better first month than the one I had planned.

---

# Addendum: three more reviews (twelve total)

## The consensus top two have changed

**Base64/hex/PEM/quoted-printable is now the single most-recommended new format** —
four of twelve models lead with it, and it is the only item every one of the last
three put first or second. The argument is uniform and strong:

- 33% inflation removed immediately, before any compression happens.
- It **scrambles byte alignment** (3 bytes → 4), which is exactly the property our
  measurements keep showing hurts byte-aligned backends.
- It appears in `.eml`/MIME, PEM certificates, JSON payloads, embedded SVG, source
  trees, and — importantly — inside XML, which recursion will expose.
- **Exactness is trivial**: canonical base64 round-trips bit-exactly; only padding
  and line-wrapping conventions need care.

**Whole gzip streams / multi-member gzip is second.** Two models flagged it
independently, and one made the sharp point: *"a lot of archives are still
effectively DEFLATE hidden inside another wrapper — .tar.gz, .npm, .crx, many
backups, many Linux packages. If your DEFLATE parser already exists, this is mostly
container work."*

That is a gap I had not checked. We handle ZIP and bare DEFLATE; **gzip carries its
own header and CRC, and multi-member gzip concatenates independent streams.** Both
are cheap extensions of machinery that already exists, and `.tar.gz` is one of the
most common things in any archive.

## Two technical answers worth keeping

### Byte-aligned CM (gemini4)

> "You will not hit 5 MB/s with bit-level context mixing and deep StateMaps. Predict
> **bytes, not bits** — this is what modern fast CM variants do to increase IPC.
> Fewer, targeted contexts: LZMA does this brilliantly by using the match byte
> rather than throwing orders 1–8 at a mixer. Vectorise the mixer — FWHT or an
> SSE-optimised linear combination, not a deep network."

This is the only concrete route anyone offered to the Q2 problem, and it reframes it:
not "PAQ but faster" but **byte-level prediction with a small number of targeted
contexts and a vectorised mixer**. It is still last in the queue, but it is no longer
obviously impossible.

### A sieve that is not the selector we disproved (gemini4)

> "Read the first 4 KB, compute a 256-bin histogram. If Shannon entropy > 7.8 the
> data is dense and no transform will pay. And cascade: run the transform attempts
> only on the first 64 KB, measure, and require at least a 5% gain before committing."

Worth distinguishing from our failed sample selector. That one chose a **backend**
from a sample and cost +2.68% size. This gates **transforms** on a *measured
mini-block result* and requires a margin before committing — closer to
verify-before-use than to prediction. The entropy gate is nearly free and would skip
the JPEG/GIF/CSV attempts on encrypted or already-compressed blocks.

## The best reframing

> "ziptastic is not 'a compressor with transforms'. It is becoming a **reversible
> container normalisation engine** with compression attached. Almost every large gain
> comes from recovering a lower-entropy representation hidden behind a container
> boundary."

That is more accurate than my own description, and it explains the whole measurement
record better than "Layer 0" does. Worth adopting in the README.

## Revised order (supersedes the table above)

| # | item | why | effort |
|---|---|---|---|
| 1 | **Recursive container descent** | multiplies every existing transform | small |
| 2 | **Base64 / hex / PEM** | consensus #1; 33% inflation, breaks alignment, trivially exact | small |
| 3 | **gzip + multi-member gzip** | `.tar.gz` everywhere; mostly container work on existing DEFLATE | small |
| 4 | **Parallelise the selector** | free speed | small |
| 5 | **WAV/PCM per-channel delta** | ~45–60% claimed | small |
| 6 | **SQLite pages** | common, public spec | medium |
| 7 | LZ4, then zstd/brotli frames, PCAP, TIFF, tar/cpio padding | real, narrower | medium |
| 8 | ECM (raw images only) | ~12%, narrow | small |
| 9 | Byte-aligned CM with SIMD mixing | single digits, still slow | large |

**Items 1–5 are all small and all multiply work already done.** The pattern across
twelve independent reviews is unambiguous: nobody argues for more entropy work, and
everyone argues for more container work.
