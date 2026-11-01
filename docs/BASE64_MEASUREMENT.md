# Base64 measured: the consensus is wrong about *why* it pays

Four of twelve models ranked base64 first, all giving the same reason: **"33%
inflation removed."** Built it and measured it. The mechanism is real; the reason is
not.

## Exactness: solved

All four real-world variants round-trip **bit-exactly**, including MIME/PEM 76-column
wrapping, single-line, CRLF line endings and stripped padding:

| variant | text | binary | exact |
|---|---|---|---|
| MIME (76-col, trailing NL) | 40,527 | 30,000 | **YES** |
| single line | 40,000 | 30,000 | **YES** |
| CRLF wrapped | 41,054 | 30,000 | **YES** |
| no padding | 40,000 | 30,000 | **YES** |

The exactness mechanism is the one that has worked every time: **decode, re-encode,
byte-compare, decline on mismatch.** Every layout variant is safe by construction
rather than by enumerating cases — the encoder records line width, EOL style and
padding presence, and the verifier catches anything it missed.

## Gain: not 25%, and it depends entirely on the payload

| payload | base64 | binary | xz(base64) | xz(binary) | gain |
|---|---|---|---|---|---|
| **random** (incompressible) | 40,527 | 30,000 | 31,460 | 30,060 | **−4.5%** |
| **structured** (byte-patterned) | 40,527 | 30,000 | 1,320 | 372 | **−71.8%** |

**On incompressible data the gain is 4.5%, not 25%.** A strong entropy coder already
recovers almost all of base64's inflation, because the 64-symbol alphabet is a
6-bit-per-symbol distribution and the coder models it well. Removing the 33%
inflation is not free money — it is money the backend was already collecting.

**The real win is alignment, and it is large.** Base64 packs 3 bytes into 4
six-bit symbols, which **destroys byte alignment**. On structured data that is
catastrophic: 1,320 bytes versus 372, a **71.8%** difference. This is the *same*
principle as the Gorilla and Rice results, arriving from the opposite direction:

> Bit-level and non-byte-aligned representations lose to byte-aligned ones against a
> strong backend. That cuts both ways — it is why Gorilla and Rice *failed*, and it
> is why base64 *pays*.

## What this changes

Base64 stays on the list, but for a different reason and with a different expectation:

- **Not** "33% for free on everything" — that is 4.5% on dense payloads.
- **Yes** on structured payloads — certificates, embedded images, JSON blobs — where
  recovering byte alignment is worth up to 72%.
- The gain is **payload-dependent**, so it must go through the same accept/reject
  trial as every other transform rather than being applied unconditionally.

Ranking it first, as four models did, overstates the common case. Ranking it below
recursion is right, because recursion is what *feeds* it structured payloads: an XML
document full of embedded base64 is exactly where this pays, and we cannot see it
until recursion exists.

**The general lesson, again:** consensus across twelve models said "33% inflation";
one measurement said "4.5% on random, 72% on structured". The models reasoned from
the format; the measurement asked where the output actually lands.
