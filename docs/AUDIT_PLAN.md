# Audit plan: compressor and decompressor

Written 2026-11-03. **Status: required before the format is called stable.** This is the
plan, not the audit — the audit itself is tracked against it section by section.

## Why this is a gate rather than a nice-to-have

`compressor.cpp` and `decompressor.cpp` are the two files every byte passes through, and
they have grown by accretion: seventeen Layer 0 transforms, nine backends, a mode selector,
two container levels, an integrity layer and a resumable parallel path. Each addition was
measured and each has tests, but **nobody has read the two files end to end since they were
one file**, and the defects found in the last week were all of the same shape — a default
that hid a path (`precomp = false`), a report field that lied for transformed streams, a
reserve that trusted a declared size, a Q parity that read the wrong bytes. None of those
was caught by a test that existed at the time; all were caught by looking.

**Running log:** `docs/AUDIT_FINDINGS.md`. Sections are closed there, not here.

## Scope

| in | out |
|---|---|
| `src/engine/compressor.cpp` (all 1,000+ lines) | `src/precompressors/*` internals, beyond their contract |
| `src/engine/decompressor.cpp` | the entropy coders (separately fuzzed and property-tested) |
| `src/engine/block_writer.cpp`, `compressor.hpp` options | CLI ergonomics |
| the format contract in `docs/FORMAT.md` against the code | performance work (own track) |

## Method

1. **Read the file top to bottom, out loud in comments.** No skimming for the interesting
   function; the defects above lived in the seams between stages.
2. **For every stage, write down its contract** — precondition, postcondition, what it does
   when it declines, and who is responsible for the size it produces.
3. **Check the contract against the code**, not against the comment.
4. **Every finding gets a test that fails before the fix**, even when the finding is
   "this is fine".
5. **Two passes, opposite directions**: encode order and decode order, checking the mirror
   relationship between them stage by stage.

## The checks, in the order they will be done

### A. The stage chain (encode)
- A1. Every stage's precondition is checked before it runs, and a stage that returns false
      leaves no partial state.
- A2. The chain's order is a *derivation*, not a history: each stage's position follows from
      what it needs to see. Write the derivation down and confirm the code matches.
- A3. Every stage's verify-by-rebuild actually compares the whole buffer, and the comparison
      is against the *input to that stage*, not the original file.
- A4. The size accounting between stages: `original_size` is the post-Layer-0 size, and every
      consumer of it knows that. (This is the field that lied once already.)

### B. The stage chain (decode)
- B1. The mirror order is exactly the reverse, and each undo is applied only when its bit is
      set.
- B2. Each undo's failure mode is `Corrupt`, not a silent pass-through.
- B3. No undo can produce a buffer larger than the stream declared, at any point.

### C. The selector
- C1. Every candidate's `compressed_size` is the size of its body, and the block header is
      written from the body that was actually kept.
- C2. A candidate that fails is dropped, never left half-built.
- C3. The chosen block is never larger than RAW, and the Tier-1 guarantee holds on the
      *total stream*, framing included.
- C4. `--max-ratio`'s second pass cannot win by accident: it replaces the stream only when
      it is strictly smaller.

### D. Options and defaults
- D1. Every option is honoured in both directions (set means run, clear means skip) and every
      option that changes the stream sets a bit the decoder reads.
- D2. **The defaults are the product.** A default that disables a whole code path is a
      coverage bug — this has bitten three times. Each default gets a test that proves the
      path runs.
- D3. The flag byte and both layer bytes are written from the same values the decoder checks;
      no bit is set speculatively.

### E. Memory and hostile input
- E1. No allocation is sized from a number the stream declares about itself (done for
      `block_count` and `dedup`; verify there is no third).
- E2. Every loop that reads a length terminates on input exhaustion, not on a counter alone.
- E3. Overflow: every `a + b` on a size or offset is checked or provably bounded.

### F. The parallel path
- F1. The parallel and sequential paths produce identical bytes for identical input, or the
      difference is documented and tested.
- F2. Shared state between block workers: list it, and show why each is safe.
- F3. The fallback thread and the native encode (the reverted experiment) leave no
      half-initialised state behind if one fails.

### G. Integrity and reporting
- G1. Every report field is true for transformed streams, not just untransformed ones.
- G2. A block failure names the block, and the digest covers the bytes that are kept.
- G3. The trailer and manifest digest are written and read from the same values.

## Evidence required to close the audit

- A written contract for every stage, in the file's own comments.
- A test per finding, failing before and passing after.
- A list of every invariant checked, with the check's location, in this document.
- The full suite green, and a fuzz run of both sides with **every transform enabled**.
- A summary in `SESSION_HANDOFF.md` saying what was read, what was found, and what was left
  unread — the last one matters as much as the others.

## What "done" does not mean

It does not mean "no bugs". It means every line was read with its contract in hand, every
finding has a test, and the parts that were not audited are named. A file this size audited
honestly will produce a list, not a clean bill of health.
