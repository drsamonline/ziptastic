# HCS third-party licence texts

Every file here was collected from a real location; nothing was written from
recollection. `INDEX.json` carries the provenance (origin path or URL) and the
SHA-256 of every file.

Two kinds of file:

- `<SPDX-ID>.txt` — the canonical licence text, fetched from the SPDX licence
  list data (`spdx/license-list-data`), the same text GitHub and SPDX publish.
- `sources/<id>-<name>` — the notice exactly as it ships with that source,
  copied verbatim from the vendored tree under `old/` where one exists,
  otherwise fetched from the upstream repository at HEAD.

Collected: 52 of 52 sources. Unresolved rows are
listed below with the reason; they are not silently treated as licensed.

## Sources

| id | SOURCES says | SPDX text | Notice file | Status |
|---|---|---|---|---|
| `xz` | 0BSD+LGPL-2.1 | [0BSD](0BSD.txt), [LGPL-2.1-only](LGPL-2.1-only.txt) | `xz-COPYING`, `xz-COPYING.0BSD`, `xz-COPYING.GPLv2`, `xz-COPYING.GPLv3`, `xz-COPYING.LGPLv2.1`, `xz-license-check.sh` | collected from vendored tree |
| `lzma` | Public-Domain | — | `lzma-HEADER.txt` | collected from a source-file header |
| `7z` | Public-Domain | — | `7z-License.txt`, `7z-copying.txt` | collected from vendored tree |
| `brotli` | MIT | [MIT](MIT.txt) | `brotli-LICENSE` | collected from vendored tree |
| `fse` | BSD-3-Clause | [BSD-3-Clause](BSD-3-Clause.txt) | `fse-LICENSE`, `fse-COPYING` | collected from vendored tree |
| `ryg_rans` | Public-Domain | — | `ryg_rans-LICENSE` | collected from vendored tree |
| `srep` | all-rights-reserved | — | `srep-HEADER.txt` | collected from a source-file header |
| `zstd` | BSD-3-Clause | [BSD-3-Clause](BSD-3-Clause.txt) | `zstd-COPYING`, `zstd-LICENSE` | collected from vendored tree |
| `lz4` | BSD-2-Clause | [BSD-2-Clause](BSD-2-Clause.txt) | `lz4-LICENSE`, `lz4-LICENSE-2`, `lz4-COPYING`, `lz4-LICENSE-3`, `lz4-COPYING-2`, `lz4-COPYING-3` | collected from vendored tree |
| `lzham` | MIT | [MIT](MIT.txt) | `lzham-LICENSE` | collected from vendored tree |
| `freearc-c` | GPL-2.0 | [GPL-2.0-only](GPL-2.0-only.txt) | `freearc-c-LICENSE` | collected from vendored tree |
| `freearc-main` | mixed | — | `freearc-main-LICENSE` | collected from vendored tree |
| `FA-master` | mixed | — | `FA-master-UNLICENSE.txt` | collected from vendored tree |
| `zpaq` | Public-Domain | — | `zpaq-COPYING` | collected from vendored tree |
| `zpaqfranz` | PD+GPL | — | `zpaqfranz-COPYING`, `zpaqfranz-LICENSE`, `zpaqfranz-LICENSE-2`, `zpaqfranz-LICENSE-3` | collected from vendored tree |
| `paq8px` | GPL-2.0 | [GPL-2.0-only](GPL-2.0-only.txt) | `paq8px-README-licence.txt` | collected from the source's README licence section |
| `LPAQ9` | GPL-2.0 | [GPL-2.0-only](GPL-2.0-only.txt) | `LPAQ9-OWNER-STATEMENT.txt` | recorded from the maintainer's statement; no text in the vault |
| `omnizip-zpaq` | MIT+Apache-2.0 | [Apache-2.0](Apache-2.0.txt), [MIT](MIT.txt) | `omnizip-zpaq-LICENSE-APACHE`, `omnizip-zpaq-LICENSE-MIT`, `omnizip-zpaq-LICENSE-NOTICE.md` | collected from vendored tree |
| `libbsc` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `libbsc-LICENSE`, `libbsc-LICENSE-2`, `libbsc-LICENSE-3` | collected from vendored tree |
| `bzip3` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `bzip3-LICENSE` | collected from vendored tree |
| `pcodec` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `pcodec-LICENSE` | collected from vendored tree |
| `bitshuffle` | MIT | [MIT](MIT.txt) | `bitshuffle-LICENSE`, `bitshuffle-LICENSE-2`, `bitshuffle-LICENSE.txt` | collected from vendored tree |
| `zxc` | BSD-3-Clause | [BSD-3-Clause](BSD-3-Clause.txt) | `zxc-LICENSE` | collected from vendored tree |
| `density` | BSD-2-Clause | [BSD-2-Clause](BSD-2-Clause.txt) | `density-LICENSE-APACHE`, `density-LICENSE-MIT` | collected from vendored tree |
| `lizard` | BSD-2-Clause | [BSD-2-Clause](BSD-2-Clause.txt) | `lizard-LICENSE`, `lizard-LICENSE-2`, `lizard-LICENSE-3`, `lizard-COPYING`, `lizard-COPYING-2` | collected from vendored tree |
| `brieflz` | zlib | [Zlib](Zlib.txt) | `brieflz-LICENSE` | collected from vendored tree |
| `snappy` | BSD-3-Clause | [BSD-3-Clause](BSD-3-Clause.txt) | `snappy-COPYING`, `snappy-COPYING.notestdata`, `snappy-LICENSE`, `snappy-LICENSE-2` | collected from vendored tree |
| `lzo` | GPL+commercial | — | `lzo-COPYING` | collected from vendored tree |
| `parmar` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `parmar-LICENSE` | collected from vendored tree |
| `shoco` | MIT | [MIT](MIT.txt) | `shoco-LICENSE` | collected from vendored tree |
| `smaz` | BSD-3-Clause | [BSD-3-Clause](BSD-3-Clause.txt) | `smaz-COPYING` | collected from vendored tree |
| `preflate` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `preflate-LICENSE` | collected from vendored tree |
| `preflate-rs` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `preflate-rs-LICENSE` | collected from vendored tree |
| `precomp-cpp` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `precomp-cpp-LICENSE`, `precomp-cpp-LICENSE-2`, `precomp-cpp-LICENSE-3`, `precomp-cpp-LICENSE-4`, `precomp-cpp-LICENSE-5`, `precomp-cpp-LICENSE-6` | collected from vendored tree |
| `packJPG` | LGPL-3.0 | [LGPL-3.0-only](LGPL-3.0-only.txt) | `packJPG-LICENSE` | collected from vendored tree |
| `aqueous` | MIT-0 | [MIT-0](MIT-0.txt) | `aqueous-LICENSE.txt`, `aqueous-LICENSE.7zip.txt`, `aqueous-LICENSE.advancecomp-7z.txt`, `aqueous-LICENSE.cryptopp.txt`, `aqueous-LICENSE.ect.txt`, `aqueous-LICENSE.fflate.txt`, `aqueous-LICENSE.go.txt`, `aqueous-LICENSE.gzip.txt`, `aqueous-LICENSE.infozip.txt`, `aqueous-LICENSE.isa-l.txt`, `aqueous-LICENSE.klauspost-compress.txt`, `aqueous-LICENSE.libdeflate.txt`, `aqueous-LICENSE.libslz.txt`, `aqueous-LICENSE.lodepng.txt`, `aqueous-LICENSE.miniz.txt`, `aqueous-LICENSE.pako.txt`, `aqueous-LICENSE.sdefl.txt`, `aqueous-LICENSE.stb.txt`, `aqueous-LICENSE-2.txt`, `aqueous-LICENSE.uzip.txt`, `aqueous-LICENSE.uzlib.txt`, `aqueous-LICENSE.wabt.txt`, `aqueous-LICENSE.zlib-chromium.txt`, `aqueous-LICENSE.zlib-cloudflare.txt`, `aqueous-LICENSE.zlib-ng.txt`, `aqueous-LICENSE.zlib.txt`, `aqueous-LICENSE.zlibjs.txt`, `aqueous-LICENSE.zopfli.txt` | collected from vendored tree |
| `rhefo` | SSPL-1.0 | [SSPL-1.0](SSPL-1.0.txt) | `rhefo-LICENSE.txt` | collected from vendored tree |
| `zopfli` | Apache-2.0 | [Apache-2.0](Apache-2.0.txt) | `zopfli-COPYING`, `zopfli-LICENSE` | collected from vendored tree |
| `advancecomp` | GPL-3.0 | [GPL-3.0-only](GPL-3.0-only.txt) | `advancecomp-COPYING`, `advancecomp-copying.txt`, `advancecomp-COPYING-2`, `advancecomp-COPYING-3` | collected from vendored tree |
| `zlib-ng` | zlib | [Zlib](Zlib.txt) | `zlib-ng-LICENSE.md` | collected from vendored tree |
| `libdeflate` | MIT | [MIT](MIT.txt) | `libdeflate-COPYING` | collected from vendored tree |
| `upx` | GPL-2.0 | [GPL-2.0-only](GPL-2.0-only.txt) | `upx-COPYING`, `upx-LICENSE`, `upx-COPYING-2`, `upx-LICENSE.txt`, `upx-LICENSE-2.txt`, `upx-COPYING-3`, `upx-COPYING-4`, `upx-LICENSE-2`, `upx-LICENSE-3` | collected from vendored tree |
| `zelf` | GPL-3.0 | [GPL-3.0-only](GPL-3.0-only.txt) | `zelf-LICENSE` | collected from vendored tree |
| `tamp` | MIT | [MIT](MIT.txt) | `tamp-LICENSE-APACHE`, `tamp-LICENSE-MIT`, `tamp-LICENSE.txt`, `tamp-LICENSE-2.txt`, `tamp-LICENSE.md`, `tamp-LICENSE-APACHE-2`, `tamp-LICENSE-MIT-2` | collected from vendored tree |
| `heatshrink` | ISC | [ISC](ISC.txt) | `heatshrink-LICENSE` | collected from vendored tree |
| `awesome-compopt` | CC | — | `awesome-compopt-LICENSE` | collected from vendored tree |
| `awesome-cpp` | CC | — | `awesome-cpp-LICENSE` | collected from vendored tree |
| `BARF` | GPL | — | `BARF-HEADER.txt` | collected from a source-file header |
| `BBB` | GPL | — | `BBB-HEADER.txt` | collected from a source-file header |
| `FLZP` | GPL-3.0-or-later | [GPL-3.0-or-later](GPL-3.0-or-later.txt) | `FLZP-HEADER.txt` | collected from a source-file header |
| `FPAQF2` | GPL | — | `FPAQF2-HEADER.txt` | collected from a source-file header |
| `SR3A` | GPL-2.0-or-later | [GPL-2.0-or-later](GPL-2.0-or-later.txt) | `SR3A-HEADER.txt` | collected from a source-file header |

## What is still open

