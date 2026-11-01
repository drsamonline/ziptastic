# Eighth batch: mostly binaries, one usable source

Ten uploaded files. Nine are executables with no source; one repository was worth
fetching.

## Licences

| item | licence | usable? |
|---|---|---|
| **ECT** (`fhanau/Efficient-Compression-Tool`) | **Apache-2.0** | **yes** — source cloned |
| Radyx 0.92 | GPL-3 | no |
| nanozip 0.09a | closed source | no |
| WABBIE, SplitSat, DeflOpt 2.07, `decomp.exe` | binaries only | no |
| `uFNV.pas` | unstated, 1.4 KB | irrelevant — we have xxhash and CRC-32 |

## ECT is the one that matters

**Apache-2.0**, 129 source files, and it does the job our preflate path cares about:
**optimal DEFLATE re-encoding**, trying multiple block-split and match-selection
strategies rather than one. It also bundles FSE under its own licence.

Why this is relevant rather than merely interesting: our ZIP/PDF path already
re-encodes DEFLATE optimally, but with **one strategy**. ECT's contribution is the
idea of trying several and keeping the smallest — the same "measure, don't guess"
policy this engine already applies at the backend level, applied one layer down
inside the DEFLATE encoder.

It is also a sanity check on zopfli, which we already vendor: if ECT beats it on our
corpus, our DEFLATE re-encoding is leaving ratio on the table, and that affects
every ZIP, PDF and PNG we touch.

## Nothing else here

- **nanozip** is a genuinely strong CM archiver and was one of the best of its era,
  but it is closed source — the binary is a benchmark reference, not a resource.
- **Radyx** is a GPL-3 GUI frontend; **WABBIE** and **SplitSat** are utilities;
  **DeflOpt** removes redundant DEFLATE blocks, which preflate already handles by
  re-encoding.

## Correcting an earlier note

In `docs/MORE_SOURCES.md` ECT was listed among "tools, not algorithms — deliberately
not pursued". That judgement was about it as a *standalone compressor*, and it
stands — but it undersold ECT's relevance as a **DEFLATE encoder strategy**, which
is a component we actually use. Recorded here rather than silently changed.
