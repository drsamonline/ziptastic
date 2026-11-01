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
