# Two questions answered: split output, and SFX

Written 2026-11-02, in answer to "can we split the ziptastic output, and create SFX".

## 1. Splitting the *output* — yes, and it is free

This is a different question from splitting the *input*, which measured at a ~2x loss
(`SPLIT_ARCHIVES.md`). Splitting the output costs nothing in ratio, because the parts
are pieces of one already-compressed stream: concatenate them and you have the exact
stream back.

What it costs is **independent decodability**: a part is not a stream, so `ziptastic d
part.001` cannot produce anything. The whole set has to be present. That is the same
trade `split(1)` makes, and it is the right one for transport (email limits, FAT32
4 GiB caps, cloud upload chunking).

**Built, same day.** `--split-size N` (suffixes K/M/G) writes `out.001`, `out.002`, …
Each part ends with a 16-byte footer — u32 index, u32 count, u32 total stream length,
`"ZS01"` — and the decoder joins the siblings itself, so `ziptastic d out.001` works
the way the name suggests. It also works when handed any other part.

Measured behaviour on a 995 KB XML file (17,859-byte stream, 4 KiB parts, 5 parts):

| | |
|---|---|
| parts written | 5 |
| decode from the first part | exact |
| decode from the last part | exact |
| one part missing | refused, named: `part 2 of 5 is missing (part.zt.002)` |

The footer is what makes that possible: without it a part is indistinguishable from a
truncated stream, and the decoder could only report `stream truncated` after joining
whatever it happened to find. The per-block SHA-256 already in the format then covers
the payload itself, so a damaged part is caught before any block is trusted.

Two limits worth stating: the suffix is three digits (`.001`–`.999`, the HJSplit
convention), so more than 999 parts is refused with a clear message rather than
silently wrapping; and a part set is not independently decodable — that is the trade,
and it is the same one `split(1)` makes.

## 2. Creating SFX — reading is done, writing is a product decision

**Reading is now supported** (this session). A self-extracting archive is a launcher
stub with the stream appended, so the magic is not at offset 0. `decompress()` now
searches for the `CHCE` magic instead of refusing the file, which means an SFX made by
concatenating any stub with a stream decodes without a special case. A false positive
costs nothing: the header parse validates magic, version, flags and layers, and the
CRC-32 of the rebuilt bytes must match at the end, so a wrong offset fails as a bad
stream rather than producing wrong data. The search is capped at the first 64 MiB.

**Writing an SFX is a different animal, and the honest answer is "not yet, and not
blindly":**

- A real SFX needs a **launcher per platform** — a PE on Windows, an ELF or a shell
  script elsewhere — and that launcher has to find the payload and call the decoder.
  That is executable code we would have to write, ship and maintain, not a format
  detail.
- **Antivirus** treats "executable with an archive appended" as a heuristic hit. Any
  SFX we ship will be flagged by some scanners, and that is a support burden, not a
  bug we can fix.
- **Signing**: an unsigned launcher on Windows produces SmartScreen warnings.
- **Licensing is fine** if we write the stub ourselves (MIT, like the rest), and not
  fine if we borrow one.

What is cheap and worth doing instead: **define the layout** — `stub | CHCE stream |
footer` with the footer carrying the stream offset and length — and ship a *plain*
launcher (a tiny shell script, or a documented 20-line C program) rather than a
Windows binary. Anyone who wants a `.exe` can wrap it with the tool of their choice,
and the format does not depend on us shipping binaries.

## Status

- SFX **reading**: shipped, with two tests (`a_stream_behind_a_stub_still_decodes`,
  `a_file_with_no_magic_is_refused`).
- SFX **writing**: not built. Layout proposal above; the blocker is the launcher, not
  the format.
- Split **output**: not built. `--split N` with a per-part footer is the shape; it is
  a writer feature and a small one.
