# Moving the precomp gate: attempted, reverted, and what it taught

Written 2026-11-03. `JPEG_NESTED.md` and `PNG_NESTED.md` both ended with the same claim: the
remaining blocker is `precomp_helps`, which decides whether to keep the inflated payload
*before* the nested transforms have had a chance to act on it. This is the attempt to fix that,
and why it was reverted.

## The restructure

`preflate_unpack` / `gzip_unpack` were made to produce a payload unconditionally, the nested
stages ran on it, and the gate was applied **after** them — so the comparison would be "what
the chain produced" against "the container", rather than "what inflation alone produced".

That is the right shape in principle. It was reverted because it did not survive contact:

- **Every case that worked still worked, identically.** ZIP of one JPEG −15.6%, ZIP of four
  JPEGs −16.5%, ZIP of GIFs −69.6% — the same numbers to the byte. So the gate had not been
  the binding constraint in any of them.
- **It broke the gzip path.** The nested stages were gated on the precomp candidate flag,
  which a `.tar.gz` never sets — preflate does not unpack a gzip — so every nested stage was
  skipped for exactly the file class the recursion exists for. Two container tests failed.
- **Fixing that produced a size regression.** With the combined flag, `nested.tar.gz` went
  50,414 → **50,799**: the gate's structural rule (`payload ≥ 2 × original → take it`) said yes
  because the inflated payload is large, and the result was worse than the input. The rule was
  written when the payload was the first-level inflation; after the nested stages it no longer
  carries the same meaning.

## What this actually establishes

**The hypothesis was wrong.** The gate was not blocking the nested-image wins — the missing
anywhere-scan was. Once PNG, JPEG and GIF could find their data inside a container payload,
every one of them fired with the gate exactly where it was.

That is worth recording precisely because two documents ended by naming the gate as the next
problem, and both were guessing from the *shape* of the code rather than from a measurement.
The measurement is: restructure the gate, re-run the three cases, observe identical numbers,
conclude the gate was not involved.

## What is still true

`precomp_helps` is a predictor, and predictors have been the source of several wrong decisions
here already (the file's own comments record three). The structural rule at its heart is blunt
enough to accept a payload that makes the output bigger — `nested.tar.gz` above — and only the
Tier-1 fallback stops that reaching the user. **That is the real open item**, and it is about
the gate's *accuracy*, not its position.

A better fix would compare the actual chosen encoding rather than a structural proxy, at the
cost of running the backend twice on a sample. Not attempted; the existing rule plus the
Tier-1 fallback has not produced a user-visible loss yet, and this turn produced no evidence
that it does.
