# Seventh batch: disc-image tools, ECM, and a note on copyleft here

Four repositories and ten uploaded files. The licences in this batch are the
strictest yet — and yet it contains the most implementable idea in several batches,
because the technique is public even though every implementation is not.

## Licences

| source | licence | usable? |
|---|---|---|
| **ECM3** (edsloter) | **AGPL-3** | **no** — the strictest copyleft; network use triggers source disclosure |
| **cmdpack** (tvtoon) | **GPL** | no |
| **Turbo-Transpose** (powturbo) | none stated | no rights |
| WinRK 3.12, DeflOpt, decomp.exe, NexoPacker | binaries only | no source |

AGPL-3 is a category stricter than the GPL items already excluded: even running a
modified version as a network service obliges you to publish it. There is no plugin
boundary that helps.

## The finding: ECM's technique is implementable from a public standard

**ECM (Error Code Modeler)** strips the error-correction data out of CD-ROM disc
images before compression, and regenerates it exactly on decode.

A CD-ROM Mode 1 sector is **2352 bytes**: 12 sync, 4 header, 2048 user data, 4 EDC,
8 reserved, 172 P-parity and 104 Q-parity. **The 4 EDC and 276 ECC bytes are fully
computable from the 2048 data bytes** — EDC is a CRC over the sector, and P/Q are
Reed-Solomon parity over GF(2^8). That is ~**12% of every sector** that carries no
information.

All of it is defined in **ECMA-130 / the Yellow Book**, which is a public standard.
So this is implementable the same way the SREP dedup stage was: from the published
specification, not from the AGPL source. The algorithms are not secret and are not
anyone's invention — they are the CD-ROM format.

**Expected gain: ~12% on `.bin`-style images before any compression**, on top of
whatever the engine then does to the 2048-byte payloads. That is the same order as
our other container wins and it is a *new container* — exactly the category that has
paid every time.

The work is bounded: an EDC CRC and a P/Q Reed-Solomon encoder over GF(2^8), a
sector splitter, and a verify-before-use round trip. No new entropy coder.

## What the other uploads are

- **DeflOpt** (binary) — removes redundant blocks from DEFLATE streams. Our preflate
  path already re-encodes optimally, so this is the same idea already covered.
- **WinRK 3.12** (binary, commercial) — a strong CM archiver. Binary only, and its
  ratio is a CM result we already know the shape of.
- **word2vec** (2 packages) — word vectors. Could seed a *word model* for a CM text
  backend, but our measured word-dictionary result was −0.38% on `dickens` and
  +0.93% on `reymont`, so the expected gain is small and language-dependent.
- **msys-1.0.10**, **decomp.exe**, **NexoPacker** — build tooling, a 4 KB LZMA
  decoder (decode *size*, not speed), and a Windows SFX tool. Nothing for us.
- **Turbo-Transpose** — a fast transpose library. Transpose is in the family we
  measured losing (+4.8% to +27.6%), so a faster implementation of a losing
  transform is still a losing transform.

## Where this leaves the plan

**ECM joins LZ4 as a container target, and it is smaller to build.** Both are
"parse a container, remove what is computable, verify the round trip". ECM's payload
is the simplest of anything on the shelf: a fixed 2352-byte sector layout and two
published algorithms.

Order I would now suggest, by value per unit of work:

1. **ECM for CD images** — ~12%, small, public spec, no new coder.
2. **LZ4** — −28.8% to −58.2% measured, larger job.
3. **Maximumcompression corpus** — so the headline number reflects container work
   at all.
4. CM backend with StateMap (UC2 as reference).
