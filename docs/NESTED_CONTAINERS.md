# Two container levels: tar inside gzip, and a ZIP that is not at offset 0

Written 2026-11-02. This is the item the gzip work left open, and it closes the
chain that `RECURSION_MEASUREMENT.md` promised at −89.4%.

## Result, through the shipped CLI, round-trip exact

| file | before this | after | `layers` |
|---|---|---|---|
| `book.tar.gz` (an `.xlsx` inside a tar inside gzip) | 75,144 | **59,183** | 0x68 |
| `mixed.tar.gz` (3 text files + the same `.xlsx`) | — | **67,553** | 0x68 |
| the `.xlsx` compressed on its own (the ideal) | 58,950 | 58,950 | — |

`book.tar.gz` went from 107,221 bytes (the file itself) to 59,183 — **−44.8%**, and
within **233 bytes** of compressing the inner `.xlsx` directly. The chain is now
reaching tar.gz → tar → ZIP → sheet XML → cell values in one pass.

`layers` 0x68 is `kLayerGzip` (0x20) + `kLayerPrecompNested` (0x40) +
`kLayerXmlNested` (0x08): three transforms stacked, undone in the reverse order.

## The two changes

**1. `scan_zip` finds local file headers anywhere, not only at offset 0.** It used to
`break` on the first byte that was not `PK\x03\x04`, which made two whole file classes
invisible:

- an **SFX archive** — an `.exe` stub followed by a ZIP, which is what Windows
  installers overwhelmingly are;
- a **`.tar.gz` holding a `.xlsx`** — the ZIP starts 512 bytes into the inflated tar.

A false positive costs nothing: `parse_stream` verifies each candidate and the payload
is rebuilt and byte-compared before it is returned.

**2. A second container level in the recursion.** The recursion used to re-enter only
the numeric transforms on what gzip inflated, so the `.xlsx` inside a `.tar.gz` was
never reached — gzip handed back a tar, and nothing looked inside the tar for another
container. Now `preflate_unpack` runs on the inflated payload as well, before the
nested numeric stage, and decode undoes it in the mirror order.

## What this cost

`kLayerPrecompNested` is bit 6, the last free layer bit. The next Layer 0 capability
needs a second `layers` byte, and that is a format change — worth knowing before the
next item rather than after.

## Tests

`tests/unit/test_containers.cpp`, three cases with fixtures embedded as byte arrays:

- `tar_inside_gzip_reaches_the_zip_inside` — asserts both `kLayerGzip` and
  `kLayerPrecompNested` are set, so a regression that stops reaching the ZIP fails
  loudly rather than quietly costing 21%.
- `a_zip_after_a_stub_is_found` — the SFX shape, asserting `kFlagPrecomp`.
- `a_plain_zip_still_works` — the offset-0 case everything already covered.

One trap worth recording: `CompressOptions::precomp` defaults to **false**, so unit
tests that do not set it never exercise any of this. The first version of these tests
failed for exactly that reason while the CLI measured the gain correctly.

## Reproducing

```bash
python3 -c "
import zipfile,tarfile,io
sheet=''.join('<row r=\"%d\"><c r=\"A%d\"><v>%d</v></c></row>'%(i,i,1000+3*i) for i in range(1,40001))
b=io.BytesIO()
with zipfile.ZipFile(b,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z: z.writestr('xl/worksheets/sheet1.xml',sheet)
open('book.xlsx','wb').write(b.getvalue())
with tarfile.open('book.tar.gz','w:gz') as t: t.add('book.xlsx')"
ziptastic c -9 -o book.tar.gz.zt book.tar.gz     # 59,183, layers=0x68
```
