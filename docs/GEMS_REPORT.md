# Deep scan of `vendor/hcs/old` — what is there, what is usable, what helps

Scope: 44 source trees, 6,449 files, 223 MB. Every tree was inventoried, every
non-empty tree was identified, licences were read, and the four most plausible
mechanisms were implemented and measured against the shipped engine.

---

## 1. The gatekeeper: licences

This decides everything. SREP already cost us a rewrite because it turned out to
be all-rights-reserved, so licences were checked before any code was read.

| tree | licence | usable? |
|---|---|---|
| `precomp-cpp-master` | Apache-2.0 | **yes** |
| `precomp-cpp-master/contrib/brunsli` | MIT (Google) | **yes** |
| `precomp-cpp-master/contrib/packjpg`, `packmp3`, `giflib`, `zlib`, `bzip2` | mixed, mostly permissive | **yes** (check each) |
| `bitshuffle-master` | MIT | **yes** |
| `brotli-master` | MIT | **yes** |
| `lz4-dev` | BSD (lib/) | **yes** |
| `libdeflate-master` | MIT | **yes** |
| `zopfli-master` | Apache-2.0 | **yes** |
| `xz-master`, `lzma`, `7z`, `libbsc-master`, `zpaq-master`, `preflate-master` | public domain / Apache / 0BSD | already in use |
| `packJPG-master` | **LGPL-3** | linkable, but only dynamically |
| `freearc-c-main` (ARC) | **GPL-2** | **no** — mechanism study only |
| `srep-main` | **no licence file** | **no** — treated as all-rights-reserved |
| `lzo-master`, `advancecomp-master`, `zstd-dev` | **GPL** | **no** |

**The headline:** ARC is GPL-2 and SREP has no licence. Both are the richest
sources of ideas here and neither can contribute code. Everything taken from them
must be reimplemented from the mechanism description, exactly as the coarse index
and dedup stage already were.

---

## 2. The big finding: our engine only understands DEFLATE

`precomp-cpp-master` (Apache-2.0, 188 source files) does what our preflate stage
does — and then five more things:

| format | our engine | precomp-cpp |
|---|---|---|
| ZIP / DEFLATE | yes (preflate) | yes |
| **JPEG** | **no** | yes — via `contrib/brunsli` (MIT) and `contrib/packjpg` |
| **MP3** | **no** | yes — via `contrib/packmp3` |
| **GIF** | **no** | yes — via `contrib/giflib` |
| **PDF** | **no** | yes (finds the DEFLATE streams inside) |
| **PNG** | partly (IDAT is DEFLATE, we handle it as a ZIP member) | yes, with full chunk handling |
| base64 | **no** | yes |

**Why this matters more than any ratio tuning:** a real photo archive is mostly
JPEG. JPEG recompression typically recovers 20-25% of a JPEG's size, because the
DCT coefficients are stored with a suboptimal Huffman table. We currently cannot
touch it. This is the largest untapped win available in the whole tree, and the
code is Apache-2.0/MIT, so it is available.

---

## 3. Mechanisms worth stealing (from GPL sources — reimplement only)

### REP (ARC, `Compression/REP`, 984 lines)
An LZ77 preprocessor for **long matches at long distances**, low memory. It is the
same class of tool as the dedup stage built in this project, but with four memory
tricks worth knowing:

- a sliding window that advances 1/16 of the buffer at a time, bounding memory;
- direct hashing without chains (1.5x less memory);
- **tag bits stored in the low bits of the hash entry**, which halves memory again;
- indexing only 1/sqrt(L) of the length-L blocks and searching only 1/sqrt(L),
  instead of rzip's 1/L — cheaper for the same long-match coverage.

Our dedup uses 16 entries per bucket with half anchored. REP's sampling scheme is
a cheaper alternative for very large inputs; worth testing if dedup is ever used
on multi-GB streams.

### DICT (ARC, `Compression/Dict`, 1,668 lines)
Word-dictionary replacement, with **prefix extension** (a frequent word absorbs
the letters that usually follow it, so "the" becomes "there"/"then"/"they" as
needed). Measured below — it works on English and hurts on Polish.

### Tornado (4,001 lines) / GRZip (3,993 lines)
Tornado is an LZ77 engine with optimal parsing — architecturally close to our own
parser, so little to gain. GRZip is a BWT+MTF+RLE chain; we use libbsc's BWT+QLFC,
which measured better than every alternative tried.

---

## 4. What was tested, and what the numbers say

All measured at 4 MiB blocks, baseline = BWT (what the engine picks for most
blocks). Negative results are as valuable as positive ones here.

### 4.1 Preprocessing that reorders bytes: loses

| transform | `x-ray` (16-bit) | `sao` (float) |
|---|---|---|
| BWT (baseline) | **3,798,258** | **4,676,596** |
| byte-shuffle 2 + BWT | 3,979,234 (+4.8%) | 5,371,210 (+14.9%) |
| byte-shuffle 4 + BWT | 4,205,524 (+10.7%) | 5,731,410 (+22.6%) |
| byte-shuffle 8 + BWT | 4,354,282 (+14.6%) | 5,969,270 (+27.6%) |
| delta 1 + BWT | 3,861,390 (+1.7%) | 5,215,020 (+11.5%) |
| delta 2 + BWT | 3,927,660 (+3.4%) | 5,628,630 (+20.4%) |

**bitshuffle's core idea — transposing typed data so like bytes group together —
makes things worse here, on exactly the data it was designed for.** The reason is
consistent with the delta result: BWT+QLFC already models the byte statistics, and
reordering destroys the local context the coder depends on. This is the clearest
negative result of the scan, and it closes off a whole family of ideas
(delta, shuffle, bit-plane split) for this engine.

### 4.2 Word-dictionary replacement: helps English, hurts Polish

| file | BWT | dict-500 + BWT | dict-500 + PPMd |
|---|---|---|---|
| `dickens` (English) | 2,359,168 | 2,353,334 (**-0.25%**) | 2,351,255 (**-0.38%**) |
| `reymont` (Polish) | 1,024,552 | 1,034,088 (+0.93%) | 1,161,675 (+13.4%) |

A 500-word dictionary with 3-byte codes and no entropy coding of the codes gives
**-0.38% on English** and loses on Polish. ARC's version is far more sophisticated
(prefix extension, dictionary sorted for its own compression, separate tuning for
PPM). As a per-block candidate the selector would take the wins and decline the
losses, so the net would be slightly positive — but at this magnitude it is not
worth the complexity yet.

### 4.3 PPMd order: content-dependent, 6 is right

Already recorded in `BENCHMARKS.md`: order 6 ties BWT on `dickens` while order 16
is 2.2% worse; on `xml` the reverse, order 6 is 30% worse than BWT and order 16
ties it. No single order wins, and a second order as a candidate buys nothing
because BWT already covers the cases where a higher order catches up.

### 4.4 LZP inside libbsc: a wash

Identical output on `x-ray`, -0.02% on `sao`, +1.9% worse on `dickens`.

---

## 5. Trees that are empty

23 of the 44 are **empty submodules** — a plain `git clone` does not fetch them:

```
aqueous awesome-compopt awesome-cpp brieflz bzip3 density fse heatshrink
lizard lzham omnizip-zpaq paq8px pcodec rhefo ryg_rans shoco smaz snappy
tamp upx zelf zpaqfranz zxc
```

Notable among them: **paq8px** (the strongest CM compressor in existence),
**bzip3** (BWT + CM, a modern bzip2 replacement), **lzham**, **snappy**,
**lizard**, **brieflz**, **upx**, **shoco/smaz** (short-string codecs).

They are empty, not absent — `git submodule update --init --recursive` fetches
them, and the repo already ships `scripts/check-vendor.sh` to verify the set. Any
future scan should run that first; otherwise two-thirds of the shelf looks bare.

---

## 6. Recommendations, ranked by expected value

1. **JPEG recompression via brunsli (MIT, vendored inside precomp).** Largest real
   win available: 20-25% on every JPEG in an archive, and we currently cannot
   touch JPEG at all. Contained the same way preflate is: a Layer 0 transform with
   a verify-before-use round trip.
2. **MP3 via packmp3, GIF via giflib, PDF stream extraction** — the rest of
   precomp's format list, once the JPEG path proves the plumbing.
3. **Word-dictionary as a per-block candidate** — small but real (-0.38% English),
   and the selector would keep it honest. Only worth it after (1) and (2).
4. **REP's memory tricks** — only if dedup is ever pointed at multi-GB streams.
5. **Materialise the 23 empty submodules** and scan paq8px and bzip3 specifically;
   bzip3 in particular is a plausible upgrade path for the BWT backend.

## 7. Cross-project notes

- The licence map in §1 applies to every project in this family, not just
  ziptastic: **ARC, SREP, LZO and advancecomp cannot be linked** by any of them.
- `precomp-cpp` + `brunsli` is the single most reusable component found — it is
  format-agnostic from the caller's side and Apache/MIT throughout.
- The negative results (§4.1) are reusable too: any project in this family that
  uses BWT+QLFC should expect delta, shuffle and bit-plane transforms to lose, and
  should not spend time on them.

---

# Part 2 — the empty submodules, fetched and measured

`git submodule update --init` was run for the two most promising of the 23 empty
trees. Both were then measured, not just read.

## bzip3 — measurably better than our BWT, and licence-blocked

`kspalaiologos/bzip3`, **LGPL-3**, one C file (`libbz3.c`), clean API.

Its pipeline is BWT + LZP + a **context-mixing order-0 arithmetic coder**. Ours is
BWT + QLFC (libbsc's adaptive coder). Measured on identical 4 MiB blocks:

| file | libbsc BWT | bzip3 | delta |
|---|---|---|---|
| `mr` | 2,249,498 | **2,151,583** | **-4.35%** |
| `x-ray` | 3,798,258 | **3,674,226** | **-3.27%** |
| `ooffice` | 2,565,414 | **2,526,126** | -1.53% |
| `dickens` | 2,359,168 | **2,330,284** | -1.22% |
| `reymont` | 1,024,552 | **1,022,729** | -0.18% |
| `osdb` | 2,347,660 | 2,355,306 | +0.33% |
| `xml` | 374,656 | 399,334 | **+6.59%** |

**bzip3 wins on five of seven files**, and the wins are large where they happen.
Offered as a per-block candidate, the selector would take every win and decline
the XML loss. Applying it to the current corpus results: `mr` -4.35%, `x-ray`
-3.27%, `dickens` -1.07%, `reymont` -0.18% — roughly 250 KB on those files alone.

**It cannot ship.** ziptastic is MIT, and its README states that only
permissively licensed sources are linked in. bzip3 is LGPL-3; static linking it
into an MIT binary imposes relinking obligations that contradict that promise.

**What to take instead:** the entire advantage is in the *entropy stage*, not the
BWT. libbsc already gives us the BWT and its inverse; what bzip3 adds is a
context-mixing coder over the transformed bytes. That is a mechanism we can write
ourselves — the engine already has rANS, a range coder and a CM backend — and it
would be licence-clean. This is the single most valuable engineering direction
found in the whole scan after JPEG.

## paq8px — the strongest CM compressor, GPL, unusable

**GPL-2+**, 135 source files, Copyright Mahoney/Osnach/Ratushnyak et al. Nothing
here can be linked into an MIT binary, and unlike bzip3 there is no single
mechanism to lift — its strength comes from hundreds of hand-tuned models.

## The pattern this scan confirms

Of the 44 trees, the ones with the best measured results are systematically the
copyleft ones:

| tree | licence | measured |
|---|---|---|
| bzip3 | LGPL-3 | **-4.35%** on mr |
| paq8px | GPL-2+ | strongest CM in existence |
| freearc (ARC) | GPL-2 | REP, DICT, Tornado |
| srep | none | the technique already reimplemented |
| lzo, advancecomp, zstd-dev | GPL | not applicable |

The permissive subset — precomp-cpp/brunsli (Apache/MIT), bitshuffle (MIT),
brotli (MIT), lz4 (BSD), libdeflate (MIT), zopfli (Apache) — is where anything we
can actually ship has to come from. That makes **reimplementation from mechanism
descriptions**, not adoption, the only route to the best ideas in this tree. The
coarse index and dedup stage already followed that route; BWT+CM would be the
next.

## Revised ranking

1. **JPEG recompression via brunsli** (MIT) — largest real-world win, licence-clean.
2. **Our own CM entropy stage on top of libbsc's BWT** — captures bzip3's
   measured -4.35% without the LGPL, using machinery the engine already has.
3. MP3/GIF/PDF via precomp's contrib.
4. Word-dictionary as a per-block candidate.
5. REP's memory tricks, only for multi-GB streams.

## Attempted: our own BWT + CM, to capture bzip3's gain licence-clean

The plan was sound — libsais gives us the BWT (already vendored and linked), and
the engine already has a binary range coder with adaptive bit models, so
BWT -> MTF -> RLE -> context-mixing range coder is buildable without touching any
copyleft code. bzip3's pipeline is exactly that shape.

**It was built and it loses.**

| file | libbsc BWT+QLFC | my BWT+MTF+RLE+CM | delta |
|---|---|---|---|
| `dickens` | 2,359,168 | 2,624,609 | **+11.25%** |
| `mr` | 2,249,498 | 2,365,481 | +5.16% |
| `x-ray` | 3,798,258 | 3,968,561 | +4.48% |

The model used was a single order-1 context (previous symbol's high nibble) over a
256-node binary tree, coded with the existing range coder. That is a fair
"obvious" implementation, and libbsc's QLFC beats it everywhere.

**What this bounds.** bzip3's -4.35% on `mr` is not available from a
straightforward context model. Getting it licence-clean needs what bzip3 actually
has: several context orders mixed together, an SSE/APM stage, and LZP
preprocessing before the BWT. That is a modelling project in its own right — weeks,
not an afternoon — and its payoff is bounded at roughly 4% on one file class.

**Recommendation changed accordingly:** BWT+CM drops from second place to "only if
someone wants to do serious modelling work". JPEG via brunsli moves further ahead
as the next real win, and it is a plumbing problem rather than a research one.

## JPEG via brunsli — proven, and it is the biggest win available

brunsli (Google, **MIT**) re-encodes a JPEG's DCT coefficients instead of
re-compressing its entropy-coded bytes. It is vendored inside precomp-cpp, and it
works.

**Round-trip is bit-exact on every test JPEG** (five in packJPG's test-files, plus
generated ones). That is the property everything else depends on.

**Measured end-to-end, through this engine:**

| image | JPEG | our engine alone | brunsli -> our engine | gain |
|---|---|---|---|---|
| `color_photo.jpg` (real photo) | 21,797 | 21,342 | **17,202** | **-19.4%** |
| synthetic gradient 900x900 | 66,744 | 53,723 | **22,664** | **-57.8%** |

The real-photo number is the one to trust: **-19.4%**, in line with the 20-25% that
JPEG recompression usually delivers. The synthetic image gains far more (-57.8%)
because a smooth gradient is nearly free in DCT space — useful as an upper bound,
not as a representative figure.

Note that brunsli's output does **not** compress further through our engine
(22,643 -> 22,664, i.e. a rounding loss): it is already entropy-coded. The entire
gain comes from replacing the JPEG with a better representation, which is exactly
what a Layer 0 transform should do.

### Build notes — the vendored copy needs two one-line patches

The brunsli copy inside precomp-cpp is **internally inconsistent**: its headers
declare `BrunsliDecodeJpeg(data, len, jpg, use_brotli)` and
`BrunsliEncodeJpeg(jpg, data, len, use_brotli)` (four arguments), but its own
`decode.cc` and `encode.cc` call them with three. It does not compile as vendored.
Adding the missing `false` argument at both call sites is enough — the parameter is
a brotli toggle and `false` matches what those call sites expect.

Second: brotli's sources are C, and must be compiled with the C compiler and linked
in. Passing them to the C++ driver fails with `-std=c++17 not allowed with C`.

With those two fixes: 24 brotli objects, 25 brunsli sources, one static binary, and
every JPEG round-trips exactly.

### What integration would involve

The same shape as preflate: detect a JPEG at a Layer 0 stage, encode with brunsli,
compress that with the normal pipeline, store the recipe, and verify the round trip
*before* committing to the transform. A new header flag bit, a mode-independent
container record, and the JPEG magic check (`FF D8 FF`) plus a brunsli success
test.

Cost is real but bounded: brunsli is ~25 files plus brotli, and it is C++11-era
code so it belongs in the C++17 shim target the way preflate and zpaq already do.

**This is the recommendation to act on.** It is the only item in the whole scan
that is simultaneously permissively licensed, measured, large, and a plumbing
problem rather than a research one.
