# Repack-tool sources: what they add

Twelve repositories from the game-repack world were added to
`vendor/hcs/old/`. This is what they contain, what is usable, and what each one
suggests for this engine or a future orchestrator.

## The headline: a roadmap of codecs we do not handle

`xtool` (Razor12911, **MIT**, 454 sources) carries a `precompressor/` directory
with real format scanners for codecs beyond DEFLATE:

| handler | lines | codec | our engine today |
|---|---|---|---|
| `PrecompLZ4.pas` | 605 | LZ4 / LZ4HC / LZ4 frame | **not handled** |
| `PrecompZSTD.pas` | 516 | Zstandard | **not handled** |
| `PrecompLZO.pas` | 480 | LZO1X | **not handled** |
| `PrecompOodle.pas` | 720 | Oodle (proprietary) | not handleable — skip |
| `PrecompEXE.pas` | 776 | executable sections | not handled |
| `PrecompZLib.pas` | — | zlib/DEFLATE | covered (preflate + our PDF path) |
| `PrecompMedia.pas`, `PrecompCrypto.pas`, `PrecompINI.pas`, `PrecompDStorage.pas` | — | media/crypto/INI/DirectStorage | out of scope |

Game archives are full of LZ4, ZSTD and Oodle, which is exactly why a repacker
needs them. **This is the same pattern that already paid for us**: precompression
of a container is worth 20–80%, while entropy tuning is worth single digits.

All three of LZ4, ZSTD and LZO have **published format specifications**
(LZ4 frame format, RFC 8878, the LZO format notes), so they can be implemented
cleanly under the same rule as everything else in
`ALGORITHMS_FROM_PAPERS.md`. What the Pascal source adds is the part the specs do
not describe: **where to look for a stream inside arbitrary bytes** — the scanning
heuristics. That is the genuinely useful contribution, and it is MIT.

## Licence position of the twelve

| repo | licence | usable? | note |
|---|---|---|---|
| **xtool** | **MIT** | **yes** | the only one that can contribute code; also the largest |
| Hakkuraifu-Repacker | GPL | no | |
| OGXRepacker | GPL | no | |
| Mini-Compressor | GPL | no | |
| freearc-old | none stated (FreeArc is GPL-2) | no | same family as the ARC already on the shelf |
| REE.PAK.Tool | none | no | format documentation only |
| PR-Pak-Tool | none | no | format documentation only |
| agsutils | none | no | |
| The-Repack-script-Repo | none (scripts) | no | |
| Crysis-Remastered | none (scripts) | no | |
| Repack-Tools | none (scripts) | no | |
| rep4cks | none (releases) | no | |

The unlicensed ones still have value as **format documentation** — RE Engine PAK,
PR PAK, OGX containers — which can be implemented from their behaviour the same way
the coarse index was implemented from SREP's published technique. No code may be
taken.

## What this changes in the plan

**For the engine (ziptastic):**

1. **LZ4 precompression** — highest value of the three. The frame format is simple,
   the streams are everywhere in game and modern container data, and it slots into
   the existing Layer 0 pattern (detect, inflate, compress the contents, keep the
   recipe, verify before use).
2. **ZSTD precompression** — RFC 8878, larger job, same shape.
3. **LZO** — worth it only if targeting older game archives specifically.

**For the orchestrator (the tool to be built later):**

`xtool` also shows the architecture worth copying *conceptually*: a registry of
precompressors, each declaring the codecs it recognises, tried in order over the
input, with the successful ones recorded so the reverse pass can rebuild exactly.
That is the same design as our Layer 0 chain, generalised to a plugin registry —
and our plugin ABI already exists for exactly this.

## Honest assessment

Nothing here beats what we already have for the Silesia corpus — these are
*container* formats, and Silesia contains none of them. Their value is entirely in
real-world archives, which is where our existing precompression already wins 20–80%.
So the right framing is not "a new ratio lever for the benchmark" but **"the next
set of formats to cover"**, and the expected gains are of the same order as JPEG
(-19%) and GIF (-29%), not of the order of the 1–3% entropy tweaks.
