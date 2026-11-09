# Third-party license texts

One file per distinct license used by the production-linked dependencies in
[`docs/specs/dependencies.md`](../docs/specs/dependencies.md). Each file starts with an
`SPDX-License-Identifier` line and the canonical source URL; the text after the blank line is the
canonical license text, unmodified. These files are inputs to the generated
[`THIRD_PARTY_LICENSES.txt`](../THIRD_PARTY_LICENSES.txt), which is produced by
[`scripts/gen_license_bundle.py`](../scripts/gen_license_bundle.py).

| File | License | Canonical source |
|---|---|---|
| [`Apache-2.0.txt`](Apache-2.0.txt) | Apache-2.0 | https://www.apache.org/licenses/LICENSE-2.0.txt |
| [`BSD-3-Clause.txt`](BSD-3-Clause.txt) | BSD-3-Clause | https://github.com/spdx/license-list-data/blob/main/text/BSD-3-Clause.txt |
| [`curl.txt`](curl.txt) | curl | https://github.com/spdx/license-list-data/blob/main/text/curl.txt |
| [`FTL.txt`](FTL.txt) | FTL (FreeType License) | https://github.com/spdx/license-list-data/blob/main/text/FTL.txt |
| [`IJG.txt`](IJG.txt) | IJG (Independent JPEG Group) | https://github.com/spdx/license-list-data/blob/main/text/IJG.txt |
| [`ISC.txt`](ISC.txt) | ISC | https://github.com/spdx/license-list-data/blob/main/text/ISC.txt |
| [`Libpng.txt`](Libpng.txt) | libpng | https://github.com/spdx/license-list-data/blob/main/text/Libpng.txt |
| [`MIT.txt`](MIT.txt) | MIT | https://github.com/spdx/license-list-data/blob/main/text/MIT.txt |
| [`MIT-0.txt`](MIT-0.txt) | MIT-0 | https://github.com/spdx/license-list-data/blob/main/text/MIT-0.txt |
| [`Public-Domain-SQLite.txt`](Public-Domain-SQLite.txt) | Public domain (SPDX: NONE) | https://www.sqlite.org/copyright.html |
| [`Unicode-3.0.txt`](Unicode-3.0.txt) | Unicode License V3 | https://www.unicode.org/license.txt |
| [`Zlib.txt`](Zlib.txt) | zlib | https://github.com/spdx/license-list-data/blob/main/text/Zlib.txt |

Notes:

- The set is derived from the dependency table, not guessed:
  `python -B scripts/gen_license_bundle.py --check` fails when a production-linked license has no
  text here, when a component has no resolved copyright holder, or when the bundle is stale.
- Dual licenses: where `docs/specs/dependencies.md` lists alternatives, the bundle ships the
  elected permissive option (miniaudio: MIT-0, rpmalloc: MIT). libjpeg-turbo ships both
  BSD-3-Clause and IJG. [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) keeps the full
  component map and the dependency table keeps the full license expression.
- Public domain is not an SPDX license and has no canonical license text. SQLite is the only
  production-linked component that is public domain without a permissive alternative, so
  `Public-Domain-SQLite.txt` quotes the authoritative SQLite public-domain dedication. If another
  pure public-domain component is linked, add its authoritative dedication and update
  `PUBLIC_DOMAIN_COMPONENTS`/`LICENSE_FILES` in the generator instead of inventing a text.
- The project's own MIT license is the repository-root [`LICENSE`](../LICENSE) file; it is
  installed next to these texts but is not part of the third-party bundle.
- These files were retrieved from the sources in the table; no text was edited or reflowed by
  hand.
