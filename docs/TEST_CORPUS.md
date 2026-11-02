# The real-world corpus, and what it measured

Written 2026-11-02. Five real images were supplied for testing. This records what each
one actually is, what the engine does with it, and which of them is useful for which
piece of work — including one assumption they disprove.

## What the files are

| file | size | what it actually is |
|---|---|---|
| `TinyIsoTest.iso` | 4,980,736 | cooked ISO9660 — 2,432 sectors of 2,048 bytes |
| `kolibrios … .iso` | 103,766,016 | cooked ISO9660 — 50,667 sectors of 2,048 bytes |
| `kolibrios … .img` | 1,474,560 | 1.44 MB floppy image (FAT12), 2,880 sectors of 512 bytes |
| `kolibrios … .distr.zip` | 53,063,864 | a ZIP of the distribution |
| `kolibrios … .raw` | 134,217,728 | **a raw disk image, not a raw CD image** |

The `.raw` was expected to be the ECM test material. It is not: 134,217,728 =
65,536 × 2,048, and it starts with `33 C0 8E D0 BC 00 7C …` — a boot sector, i.e. x86
code — not the `00 FF×10 00` sync pattern of a 2,352-byte CD sector. **ECM still has no
test material**: it needs a raw Mode-1 CD image, which none of these are.

## Measured, `-9`, round-trip verified

Re-measured on 2026-11-02 after ECM, PNG and BMP landed — identical to the first run, so
none of the new stages disturbed a file they do not apply to.

| file | size | compressed | gain | time | `layers`/`layers2` |
|---|---|---|---|---|---|
| `TinyIsoTest.iso` | 4,980,736 | **3,333,900** | **−33.1%** | 36 s | 0x00 / 0x00 |
| `kolibrios … .img` | 1,474,560 | **1,257,827** | **−14.7%** | 8 s | 0x00 / 0x00 |

Both round-trip byte-exact. Neither fires a Layer 0 transform — a cooked ISO and a FAT12
floppy have no DEFLATE, no base64, no PCM, no PNG, no BMP and no CD sectors in them, so
the backend does all the work. That is the correct answer, not a missing feature.

The timings are worth recording: **~0.14 MB/s on this box at `-9`**. That is the number
that decides what can be measured in a single 300 s call — anything over about 40 MB is
out of reach, which is why the 53 MB ZIP and the 99 MB and 128 MB images remain
unmeasured.

## What did not finish, and why

**A slice is not a substitute, and that is worth knowing.** Taking the first 8 MB of the
ZIP and compressing it gives 7,933,212 bytes with precomp **on or off, identically** —
`layers 0x00/0x00`. That is not precomp failing; a slice cuts through members, so the
DEFLATE streams it finds are truncated and no member can be inflated. A container
transform can only be measured on whole members, which means the whole file, which means
more than one call can give.

`… .distr.zip` (53 MB) did not complete in a 260 s run at default settings. On this box
the engine runs at roughly 0.3–0.6 MB/s, so a 53 MB archive needs several minutes before
precomp has even finished inflating it. The 99 MB ISO and the 128 MB raw image are out of
reach for a single 300 s call for the same reason.

**This is a sandbox constraint, not an engine defect**, but it is worth stating plainly:
every large-file number in this repository was measured on fixtures of a few MB, and the
corpus above has not been measured end to end. A machine with more time per call — or a
`bench` mode that reports after each block — is what that needs.

## What these files are good for

- **`TinyIsoTest.iso` and the `.img`** are honest end-to-end round-trip tests on real
  binary images, which the synthetic fixtures are not. They belong in a corpus test.
- **The `.distr.zip`** is the best available test of the ZIP path on a real archive —
  dozens of members, real DEFLATE, mixed content — and it is the one worth running on a
  machine that can give it ten minutes.
- **The two ISOs** would exercise the precomp scanner at scale.
- **None of them can validate ECM.** For that we need a `.bin`/`.raw` ripped from a CD
  with 2,352-byte sectors; `cdrdao`, `dd` on a CD device, or any game-repack source would
  provide one.

## Status

Corpus recorded. Two files measured, three unmeasured for time. ECM remains blocked on
test material rather than on code.
