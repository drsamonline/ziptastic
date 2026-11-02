# What game repacking practice says about this design

Recorded 2026-11-02 from the project author's own workflow: years of producing
game repacks by hand, plus the imported scripts that ship with repack tools. This
is field knowledge, not measurement — it is written down because it predicts
which of our ideas are worth building and which are already answered.

## The pipeline that has worked for years

```
zip store  ->  precomp  ->  srep  ->  7z   or   arc
```

with two observations attached to it:

- **7z and arc trade places.** Sometimes arc wins, sometimes 7z, on the same job.
- **precomp is often useless and slow.** On intense settings it costs a lot of
  time for nothing on many inputs; on games it is frequently worth it. Which is
  which is not predictable from the file type.
- **Repack scripts differ per game.** The imported scripts use different
  combinations for different titles, which is the strongest possible statement
  that no single chain is right.

## Stage by stage, against what ziptastic already has

| step | what it is for | ziptastic equivalent | state |
|---|---|---|---|
| **zip store** | get every file into one container *without* compressing, so the next stages see raw bytes | Layer 0 container descent | **gap: stored entries are invisible** |
| **precomp** | find embedded DEFLATE streams, inflate them, keep a recipe that rebuilds them exactly | `preflate` + PDF + ZIP + gzip | **shipped**; gzip added today |
| **srep** | remove long-range redundancy between similar blocks, before the entropy coder | `dedup` (coarse, SREP-style) | **shipped**; SREP itself is unlicensable |
| **7z / arc** | the strong final coder | LZMA2 (Tier-1), PPMd, BWT, CM/zpaq | **shipped**, and chosen **per block** |

**The gap is the first step, and it is a real one.** `scan_zip` in
`preflate_wrap.cpp` accepts only `method == 8` (DEFLATE) and stops at the first
entry that is not a local file header. A zip built with `-0`/store has every entry
at `method == 0`, so ziptastic walks past all of it and compresses the container as
opaque bytes. That is exactly backwards from the pipeline above, where "zip store"
exists *to make the contents visible*.

## What this practice confirms

**precomp is a gamble and must be gated.** "Often useless and slow, sometimes
worth it" is the same shape as our own record: the transform pays only when there
is DEFLATE to remove, and our gate has been wrong three times in the direction that
costs ratio. The lesson the field already knows — never run it blindly on intense
settings — is the one this codebase arrived at from the other direction.

**"Sometimes arc, sometimes 7z" is the per-block selector's argument.** Picking a
backend per file is what the repackers do by hand, by trial. We pick per 4 MiB
block, which is strictly finer, and we pick between BWT, PPMd, rANS, LZMA2 and CM
rather than two fixed tools.

## What it reveals about ordering

The pipeline is a *sequence*, and each stage changes what the next one can see:

1. store — so nothing is hidden behind DEFLATE
2. precomp — so embedded streams become plain bytes
3. srep — so long-range duplicates across those bytes disappear
4. strong coder — so what is left is coded once

ziptastic's Layer 0 order is already close: base64 → numeric/XML → JPEG → GIF →
DEFLATE/gzip → recursion → BCJ → dedup → per-block backends. **dedup is last of the
Layer 0 stages, which matches srep's position**; the recursion is what the repack
pipeline achieves by hand when it unpacks, precomps, and re-packs.

The one structural difference: repackers get a **second pass** — unpack, precomp,
then pack again — and we only recurse once. That is the tar-inside-gzip item.

## Where each piece belongs

**ziptastic** (links, permissive only):
- stored-ZIP unpacking, i.e. the "zip store" step — small, and it is the entry
  point of the whole pipeline.
- tar parsing, so the gzip recursion can reach the archive inside.
- SFX and split archives (see `SESSION_HANDOFF.md` §6b/6c).

**heavenzip** (spawns binaries, any licence):
- **pipeline search.** If different games want different combinations, the honest
  answer is to try the combinations and measure — which is what heavenzip is for,
  and what ziptastic cannot do because SREP and ARC are not linkable.
- per-title profiles: record which combination won for which input, so the next
  run starts from the winner instead of from scratch.
- driving the real SREP binary, which ziptastic may never ship.

That split is the same argument as `SESSION_HANDOFF.md` §1: ziptastic links, so
permissive only; heavenzip spawns, so the ceiling is much higher.

## The experiments this suggests, in order

1. **Stored-ZIP unpacking, measured.** Build a `zip -0` container of a mixed game-like
   tree, compress it today (expected: no gain, contents invisible), then with the
   entries unpacked. If the pipeline above is right, this is where the gain starts.
2. **Second-level recursion through tar.** Already the top item; this is what the
   repackers' unpack-repack cycle buys.
3. **Pipeline search in heavenzip.** Once the primitives exist, enumerate
   {store, deflate} × {precomp on/off} × {dedup params} × {7z, arc, zpaq} on a
   handful of real inputs and record the winners. That turns "different scripts per
   game" into a table.

Nothing in this document has been measured yet — it is the field knowledge written
down so the next session starts from it rather than rediscovering it. The
stored-ZIP gap, however, is a code fact: `preflate_wrap.cpp` line 92 accepts only
`method == 8`.
