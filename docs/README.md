# The docs, and which of them you can trust

Sixty-five markdown files, written over six days, and until now nothing said which were still true.
This page does. It is a map, not a rulebook: the rules live in the files it points at.

**The one rule that matters: [`FINDINGS.md`](FINDINGS.md) is the authority for every number.** Where
any other document disagrees with it, `FINDINGS.md` is right and the other document is a record of
what was believed at the time. Several files say so in their own first lines; the rest are listed
below so you do not have to check.

## Start here

| file | what it is |
|---|---|
| [`FINDINGS.md`](FINDINGS.md) | **The authority.** Every measurement, every closed hypothesis, and why. |
| [`ENGINE_STATE.md`](ENGINE_STATE.md) | The map of the engine: layers, stages, what runs when. |
| [`FORMAT.md`](FORMAT.md) | The on-disk format, v8, and the policy for changing it. |
| [`BUILD.md`](BUILD.md) | Building it, testing it, the toolchain traps, the marker convention. |
| [`RELEASE_PLAN.md`](RELEASE_PLAN.md) | What is left, in batches ordered by cost. |
| [`NEW_SESSION_PROMPT.md`](NEW_SESSION_PROMPT.md) | The handover: what a fresh session should know first. |
| [`VALIDATION.md`](VALIDATION.md) | How the engine is proved correct rather than assumed correct. |

## Current: design and reference

These describe how things are, and are kept up to date.

[`ALGORITHMS_FROM_PAPERS.md`](ALGORITHMS_FROM_PAPERS.md) ·
[`BENCHMARK_SOURCES_2026-11-07.md`](BENCHMARK_SOURCES_2026-11-07.md) ·
[`COMPETING_TOOLS.md`](COMPETING_TOOLS.md) ·
[`CONCURRENT_FALLBACK.md`](CONCURRENT_FALLBACK.md) ·
[`HATCHING.md`](HATCHING.md) ·
[`INTEGRITY.md`](INTEGRITY.md) ·
[`LICENCES.md`](LICENCES.md) ·
[`NESTED_CONTAINERS.md`](NESTED_CONTAINERS.md) ·
[`REPACK_PIPELINES.md`](REPACK_PIPELINES.md) ·
[`SELECTOR_PROFILE.md`](SELECTOR_PROFILE.md) ·
[`SOURCES_REVIEW.md`](SOURCES_REVIEW.md) ·
[`TEST_CORPUS.md`](TEST_CORPUS.md) ·
[`TOOL_SURVEY_2026-11-07.md`](TOOL_SURVEY_2026-11-07.md)

Three of those — `COMPETING_TOOLS`, `SELECTOR_PROFILE`, `TEST_CORPUS` — say "superseded for current
figures" in their first line. Read that as *the design is current, the numbers are in
`FINDINGS.md`*, not as "throw this away".

## Working records: what was measured, and how

The measurement log for each transform, in the order it happened. Current for *how* a number was
arrived at; the number itself may have moved.

[`BASE64_INTEGRATION.md`](BASE64_INTEGRATION.md) ·
[`BASE64_MEASUREMENT.md`](BASE64_MEASUREMENT.md) ·
[`BASE64_NESTED.md`](BASE64_NESTED.md) ·
[`BMP_MEASUREMENT.md`](BMP_MEASUREMENT.md) ·
[`CM_BACKEND_MEASUREMENT.md`](CM_BACKEND_MEASUREMENT.md) ·
[`ECM_IMPLEMENTATION.md`](ECM_IMPLEMENTATION.md) ·
[`ECM_MEASUREMENT.md`](ECM_MEASUREMENT.md) ·
[`ECM_TEST_MATERIAL.md`](ECM_TEST_MATERIAL.md) ·
[`GIF_NESTED.md`](GIF_NESTED.md) ·
[`GZIP_INTEGRATION.md`](GZIP_INTEGRATION.md) ·
[`HUFFMAN_GATE.md`](HUFFMAN_GATE.md) ·
[`JPEG_NESTED.md`](JPEG_NESTED.md) ·
[`LZ4_MEASURED.md`](LZ4_MEASURED.md) ·
[`NUMERIC_RESIDUAL_EXPERIMENT.md`](NUMERIC_RESIDUAL_EXPERIMENT.md) ·
[`PNG_MEASUREMENT.md`](PNG_MEASUREMENT.md) ·
[`PNG_NESTED.md`](PNG_NESTED.md) ·
[`PRECOMP_GATE.md`](PRECOMP_GATE.md) ·
[`RECURSION_MEASUREMENT.md`](RECURSION_MEASUREMENT.md) ·
[`SQLITE_MEASUREMENT.md`](SQLITE_MEASUREMENT.md) ·
[`TIER0_GUARD.md`](TIER0_GUARD.md) ·
[`WAV_MEASUREMENT.md`](WAV_MEASUREMENT.md) ·
[`XML_NUMERIC_MEASUREMENT.md`](XML_NUMERIC_MEASUREMENT.md) ·
[`ZIP_STORE_MEASURED.md`](ZIP_STORE_MEASURED.md) ·
[`BENCHMARKS.md`](BENCHMARKS.md)

## Where the third-party sources came from

The reading list behind the vendor tree. [`vendor/hcs/SOURCES`](../vendor/hcs/SOURCES) is the
machine-readable version; these are the surveys that produced it.

[`AV_SOURCES.md`](AV_SOURCES.md) ·
[`BINARY_TOOLS_BATCH.md`](BINARY_TOOLS_BATCH.md) ·
[`DISC_IMAGE_SOURCES.md`](DISC_IMAGE_SOURCES.md) ·
[`ENCODE_SU_SOURCES.md`](ENCODE_SU_SOURCES.md) ·
[`GEMS_REPORT.md`](GEMS_REPORT.md) ·
[`MORE_SOURCES.md`](MORE_SOURCES.md) ·
[`PACKER_SOURCES.md`](PACKER_SOURCES.md) ·
[`REPACK_SOURCES.md`](REPACK_SOURCES.md) ·
[`SCIENTIFIC_ML_SOURCES.md`](SCIENTIFIC_ML_SOURCES.md) ·
[`AI_REVIEWS_SYNTHESIS.md`](AI_REVIEWS_SYNTHESIS.md) ·
[`LEXORA_ASSESSMENT.md`](LEXORA_ASSESSMENT.md) ·
[`STAC_V2_REVIEW.md`](STAC_V2_REVIEW.md)

## Historical: kept because deleting evidence is worse

Format versions that no longer exist, and plans that were superseded. Read them for the reasoning,
never for a number.

[`FORMAT_V6.md`](FORMAT_V6.md) ·
[`FORMAT_V7.md`](FORMAT_V7.md) ·
[`FORMAT_V8.md`](FORMAT_V8.md) ·
[`AUDIT_PLAN.md`](AUDIT_PLAN.md) ·
[`AUDIT_FINDINGS.md`](AUDIT_FINDINGS.md) ·
[`SESSION_HANDOFF.md`](SESSION_HANDOFF.md) ·
[`SFX_AND_SPLIT_OUTPUT.md`](SFX_AND_SPLIT_OUTPUT.md) ·
[`SPLIT_ARCHIVES.md`](SPLIT_ARCHIVES.md) ·
[`VS_THE_FIELD.md`](VS_THE_FIELD.md)

## Not documentation

`404.html` is the GitHub Pages 404 and `index.html` is the landing page. Both are built artefacts.
