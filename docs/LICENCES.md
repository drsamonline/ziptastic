# Licences

ziptastic itself is **MIT** (see `LICENSE`). It links, statically, a set of vendored
compressors — and one of them is the reason this file exists rather than a footnote.

## Vendored sources

`vendor/hcs/SOURCES` is the machine-readable manifest: one row per source, with its upstream URL,
the pinned commit, the SPDX licence and how it is integrated (`link`, `copy-src`, `reference`).
That file is the authority; this page is the summary a reader needs before shipping a binary.

| component | licence | how it is used |
|---|---|---|
| xz / liblzma | 0BSD **and** LGPL-2.1 | linked; the Tier-1 LZMA2 fallback |
| brotli | MIT | linked; a per-block backend |
| brunsli | MIT | linked; the JPEG transform |
| libbsc | Apache-2.0 | linked; the BWT backend |
| zpaq | public domain | linked; the context-mixing backend |
| ppmd | public domain | linked; the PPMd backend |
| preflate | Apache-2.0 | linked; the DEFLATE recompression |

## What the LGPL part means for a binary you ship

liblzma is dual-licensed 0BSD **or** LGPL-2.1, and this build takes the 0BSD arm for the library
itself wherever that is possible. If you redistribute a **statically linked** binary and prefer to
rely on the LGPL arm for any part of it, LGPL-2.1 §6 asks you to offer the means to relink against
a modified version — in practice: ship the object files, or ship the source and the build recipe.

That is why `docs/BUILD.md` exists in this detail: **the recipe is the compliance artefact.** A
build that cannot be reproduced from the published source cannot satisfy a relink obligation, and
a build whose recipe lives in a session log is not published.

## Attribution

The vendored sources keep their own licence files inside `vendor/hcs/old/<name>/`. Nothing here
overrides them; where this page and a vendored `LICENSE` disagree, the vendored file wins.
