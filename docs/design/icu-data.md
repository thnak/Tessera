# ICU reduced static data: measured cost (spike task-94)

Status: measured spike, night of 2026-11-10. This note holds the method, the full measurement
tables, the failure-mode experiment, and the follow-up slice for the ICU-data question;
[`text.md`](text.md) sections 12 and 13 hold the summary and the GO decision. Nothing here is on
by default: the data step is behind `TESSERA_ICU_REDUCED_DATA=OFF`, so the default build and CI
are unchanged.

Terms: ICU = International Components for Unicode; CLDR = the locale data ICU packages;
`icudt<version><type>.dat` = an ICU data package; ToC = the package's table of contents;
`icupkg` = ICU's data-package tool; blob = a data package image linked into the app.

## 1. Why this note exists

`dependencies.md` lists ICU as "V1 hard" and warns "do not ship the full data set blindly";
ADR-012 requires embedded static data; `text.md` section 12 asked whether to subset. None of
that had a measurement. The facts that shaped the spike:

- ICU is **not vendored and not fetched**: no `FetchContent`, no `find_package`, no target, no
  version pin, and **zero call sites** in `src/`, `include/`, `tools/`, or `tests/`. HarfBuzz
  builds with `HB_HAVE_ICU=OFF`.
- The three consumers named in the decision are unimplemented: text segmentation and line
  breaking use bespoke `ui/text/edit_grapheme.*` helpers (combining marks, ZWJ, regional
  indicators) plus greedy wrap; `services/i18n` (locale/formatters) is spec-only; collation is
  optional and unused.
- Windows ships a system `icu.dll` (2.8 MiB, no separate data library). It is a system
  component and cannot satisfy ADR-012's self-contained-static promise.

So ICU's V1 value is the **Unicode core** (break iteration, normalization, case mapping,
properties) plus **per-locale UI data** (months, date/number patterns, currency, region names).

## 2. Method and pinned inputs

- ICU **76.1** (2024-10), contemporaneous with the pinned FreeType 2.13.3 / HarfBuzz 10.2.0 /
  SheenBidi 3.0.0 set. Inputs: `icu4c-76_1-Win64-MSVC2022.zip` (tools, headers, DLLs) and
  `icu4c-76_1-data-bin-l.zip` (`icudt76l.dat`, 31,851,856 B).
- Reduction with the pinned upstream `icupkg`: list the package, then remove every resource
  bundle whose locale is not in the documented list plus the ancestors a listed locale falls
  back through. Every locale-independent item stays (Unicode tables, break rules and
  dictionaries, normalization, converters, transliteration, spellout, collation).
- The locale-name test is `^[a-z][a-z][a-z]?(_[A-Za-z0-9]*)*$` (CMake's regex engine has no
  `{m,n}` repetition). ICU's non-locale bundles (`pool`, `icustd`, `icuver`, `supplementalData`,
  `zoneinfo64`, ...) do not match it and are never removed.
- `icupkg` refuses a removal that orphans a dependent item (`en_GB` needs `en_001`); the step
  keeps the named dependency and retries, so the keep set only grows and the loop terminates.
- Windows note: the common-data ToC prefix must match `U_ICUDATA_NAME` (`icudt76l` for a `.dat`,
  `icudt76` for a Windows data DLL). `icupkg --toc_prefix` rewrites it; the platform-specific
  prefix belongs in the ICU build slice, not in the trimming step.

Machine: Windows 11, clang 22.1.5, CMake 4.1.1, Ninja, `-j12`, `TESSERA_BUILD_VULKAN=OFF`,
`TESSERA_BUILD_POC=OFF`. Timings are wall clock on a warm machine, one run per cell.

## 3. Measured cost

Blob sizes are the data package. "Link delta" is a real executable linked with the `genccode`
data object for that tier (`icudt76l_dat.obj`, the artifact a static data build links) minus the
same executable without it.

| Tier | Locales kept | Items | Blob bytes | MiB | Of full | Link delta |
|---|---|---|---|---|---|---|
| Full upstream package | 924 | 4,136 | 31,851,856 | 30.37 | 100% | +31,839,232 |
| Base: `root` only, all non-locale data | 1 | 269 | 13,633,072 | 13.00 | 42.8% | not measured |
| Base + `en` | 2 | 279 | 13,737,088 | 13.10 | 43.1% | not measured |
| Recommended (22 listed ids, 23 after closure) | 23 | 447 | 17,293,904 | 16.49 | 54.3% | +17,281,024 |
| Recommended, converters dropped | 23 | 257 | 12,136,048 | 11.57 | 38.1% | not measured |
| Recommended aggressive (+ translit, rbnf, coll) | 23 | 213 | 8,593,024 | 8.19 | 27.0% | +8,580,096 |
| Base + `en`, aggressive | 2 | 82 | 6,539,328 | 6.24 | 20.5% | +6,526,464 |

Interpretation:

- **The bulk is not locale data.** 42.8% of the package is locale-independent; 924 locale packs
  are 57%, about **169 KiB per locale** at the recommended tier and **~103 KiB** once collation
  is dropped (`coll/<locale>.res` is the heaviest per-locale bundle).
- **The floor is Unicode.** 13.63 MiB of `unames.icu`, normalization `.nrm`, break rules `.brk`,
  CJK/Thai/Lao/Khmer/Burmese `.dict`, `pool.res`, plus ~200 legacy charset converters `.cnv`.
- **Trimming to ~6-8 MiB needs the optional packages dropped:** 5.16 MiB of converters, 2.6 MiB
  of transliteration + spellout + collation. A UTF-8-only UI framework needs neither in V1.
- **Configure/build cost is negligible.** Warm reconfigure 3.6 s (OFF) vs 3.8 s (ON), delta
  **+0.20 s**; the `tessera_icu_reduced_data` target takes **1.0 s** for the whole 31.9 MB
  package. A cold lane configure is 45.7 s either way (FetchContent dominates).
- **Binary delta equals the blob** (the object is copied verbatim into the image): +17.28 MB
  recommended, +8.58 MB aggressive, +6.53 MB en-only aggressive, +31.84 MB full. A data DLL built
  from the same object is object size + ~106 KiB of PE/DLL overhead.

## 4. The build step (opt-in, default OFF)

`third_party/icu/{CMakeLists.txt,reduced_data.cmake,locales.txt}`, declared from
`third_party/CMakeLists.txt` so the spike's diff stays out of the root script.

```text
option(TESSERA_ICU_REDUCED_DATA  OFF)      # the only switch; CI never sets it
TESSERA_ICU_TOOLS_DIR            pinned ICU bin/ directory containing icupkg
TESSERA_ICU_FULL_DATA            pinned icudt<version><type>.dat package
target tessera_icu_reduced_data  writes ${CMAKE_BINARY_DIR}/icu/icudt76l.dat
```

`locales.txt` is the documented locale list (one ICU id per line, `#` comments). A typo fails the
build with the missing `<locale>.res` item; an orphaned fallback bundle is repaired by keeping the
dependency; a stale `locales.txt` can never ship silently.

```powershell
cmake --preset dev -B build/lane-icu -DTESSERA_BUILD_VULKAN=OFF -DTESSERA_BUILD_POC=OFF `
  -DTESSERA_ICU_REDUCED_DATA=ON "-DTESSERA_ICU_TOOLS_DIR=<icu>/bin64" `
  "-DTESSERA_ICU_FULL_DATA=<icu>/icudt76l.dat"
cmake --build build/lane-icu --target tessera_icu_reduced_data
```

Result on the pinned package: 3,689 items removed, 447 kept, 17,293,904 bytes (54.3%) in 1.0 s.

## 5. Failure modes when the subset lacks a locale

Documented behaviour (ICU 76): a locale resolves through its fallback chain (`de_DE` -> `de` ->
`root`) and reports through `UErrorCode`. Resource-bundle APIs return
`U_MISSING_RESOURCE_ERROR` for an absent bundle; formatters and break iterators usually succeed
against `root` with `U_USING_DEFAULT_WARNING`/`U_USING_FALLBACK_WARNING`.

Experiment: `third_party/icu/experiment/icu_subset_probe.c`, a spike-only harness linked against
the pinned ICU 76.1 DLLs. ICU only sees the package ToC, so a locale trimmed out of the package
and a locale that was never in it are the same code path; the run asks for `zz_ZZ` and compares
it with `ja_JP`:

| Probe | `zz_ZZ` (absent) | `ja_JP` (present) |
|---|---|---|
| locale bundle | `U_USING_DEFAULT_WARNING`, actual locale empty | `U_ZERO_ERROR`, actual locale `ja_JP` |
| `ubrk_open(LINE/WORD)` | ok, `U_USING_DEFAULT_WARNING`, root rules | ok, fallback warning (no `ja` line tailoring) |
| `unum_open(DECIMAL)` | `U_USING_DEFAULT_WARNING`, `1,234,567.5` | same value, actual locale `ja_JP` |
| `udat_open(DEFAULT)` | **`U_ZERO_ERROR`**, root pattern `2024 M01 1` | `U_ZERO_ERROR`, `2024/01/01` |
| `ucol_open` | ok, `U_USING_DEFAULT_WARNING`, root order | ok, root order (no `ja` collation) |
| case mapping `i` -> upper | `I` (root) | `I` (root; `tr` needs `tr` data for `İ`) |
| NFC + code point names | `U_ZERO_ERROR` | `U_ZERO_ERROR` |

What this means:

1. **Silent wrong formatting is the risk, not a crash.** `udat_open`/`unum_open` can return
   `U_ZERO_ERROR` while producing root-formatted output, so a missing locale pack shows wrong
   dates/numbers with no diagnostic; the framework must not rely on ICU status codes to detect
   a missing pack.
2. **Text APIs degrade gracefully**: break iteration, word segmentation, and collation fall back
   to root, and Thai/Lao/Khmer/Burmese dictionary segmentation keeps working because those
   dictionaries are locale-independent and always kept.
3. **Unicode core is unaffected** by locale trimming; normalization, case mapping, and character
   data always work because that data is never removed.
4. Mitigation: ship a kept-locale manifest generated by the same step and have `LocaleResolver`
   validate against it, emitting a diagnostic instead of falling back silently (follow-up).

Limitation: the prebuilt Windows ICU ships only import libraries whose data comes from the data
DLL, so `udata_setCommonData` cannot override it in-process; the reduced-blob run needs the static
ICU build the follow-up slice adds. The ToC diff proves the items are gone; the probe proves what
ICU does when they are.

## 6. Decision and follow-up

The decision (GO with locale packs, the recommended locale set, and the CI cost) is recorded in
[`text.md`](text.md) section 13. The follow-up implementation slice that was planned here landed
in task-101; section 8 records what shipped, the measured cost, and the one deviation from the plan
below (the data step still trims the pinned package because `pkgdata --filter-file` needs CLDR
source data the pinned archive does not contain).

1. `third_party/CMakeLists.txt`: FetchContent ICU 76.1 (pinned archive hash) with
   `BUILD_SHARED_LIBS=OFF`, `U_STATIC_IMPLEMENTATION`, converters/transliteration/IO disabled,
   and `pkgdata --filter-file` for the data so rules and per-locale bundle tails are pruned too
   (strictly better than `icupkg` removal). Replace `reduced_data.cmake` with the filter file,
   keep `locales.txt` as its source of truth, and keep the option OFF.
2. `tessera_icu` INTERFACE target + `udata_setCommonData(<linked blob>)` at startup (ADR-012
   rule 5), with a pinned-version assertion.
3. Replace the bespoke `edit_grapheme.*` word/line helpers and greedy wrap with `ubrk_*`
   line/word/sentence iterators; add Arabic/Thai/CJK segmentation tests.
4. Generate `locales.json` (kept ids + blob hash) from the same step, and have `LocaleResolver`
   validate against it and emit a structured diagnostic on a miss.
5. Test: `icu_locale_pack` opens a locale that is not in the manifest, asserts the diagnostic and
   root fallback, and golden-tests `udat`/`unum` output for two kept locales.

## 7. Reproducing

```text
icu4c-76_1-Win64-MSVC2022.zip  (tools: icupkg, genccode, headers, DLLs)
icu4c-76_1-data-bin-l.zip      (icudt76l.dat, 31,851,856 B)
third_party/icu/experiment/icu_subset_probe.c   (fallback-path harness, not built by CMake)
```

The probe is spike-only: never compiled by the Tessera build, never linked into the framework; it
links the pinned ICU 76.1 DLLs purely to observe `UErrorCode` and formatted output for a locale
set. The section 4 commands plus the `icupkg -r` / `genccode -o` runs described in sections 2-3
reproduce every number above.

## 8. Implementation (task-101): static ICU with the reduced blob

Status: landed behind the same `TESSERA_ICU_REDUCED_DATA=OFF` switch. The default build and every
default lane stay on the builtin engine; the dedicated Linux `icu-text` CI job
(`.github/workflows/linux.yml`) provisions the pinned host tools and data, configures with the
option ON, and runs the filtered text suite, so the ICU engine is proven on every push without
linking the blob into every job (see section 8.6). The greedy wrap consumes UAX #14 segments in both
configurations (`text.md` section 6), so nothing in the tree needs the option to stay OFF any more.

### 8.1 Build shape

ICU upstream ships **no CMake build** (`icu4c/source/CMakeLists.txt` does not exist in any release,
including 76.1 and 77.1): it is autoconf on POSIX and MSBuild on Windows. Combined with the measured
limitation that the prebuilt Windows import libraries cannot override their data DLL (section 5),
a static ICU can only come from compiling the pinned sources, so `third_party/icu/CMakeLists.txt`
does exactly that, the same approach `cmake/ThorVG.cmake` uses for the other non-CMake upstream:

```text
tessera_icu_common   static, 201 TUs from source/common (U_COMMON_IMPLEMENTATION)
tessera_icu_i18n     static, 254 TUs from source/i18n   (U_I18N_IMPLEMENTATION)
tessera_icu_data     static, the genccode object for the trimmed package
tessera_icu          INTERFACE: headers, U_STATIC_IMPLEMENTATION, version/data pins
tessera_icu_manifest custom target: <build>/icu/{locales.json,icu_locale_manifest.h}
```

ICU 76 is a C++17 code base, so the third-party targets pin `CXX_STANDARD 17` and compile with
`-w` (`/W0` on MSVC); they never inherit the project's `-Werror` policy (AGENTS.md "warnings").

The sources come from the pinned `icu4c-76_1-src.tgz` (SHA-256
`dfacb46b…30f4f3e`), fetched by `FetchContent` like FreeType/HarfBuzz/SheenBidi;
`TESSERA_ICU_SOURCE_DIR` points at a pre-fetched tree for offline runs and is version-checked
against the pinned `76.1.0`.

### 8.2 Data step and the two names ICU requires

The pinned package cannot be rebuilt with `pkgdata --filter-file`: `pkgdata` packages CLDR *source*
data, and the release archive ships only a prebuilt `source/data/in/icudt76l.dat` — there is no
`source/data/locales/*.txt` (2 entries match `data/locales`, both directories, zero locale files).
Trimming the pinned package with `icupkg` is therefore retained from the spike, unchanged in cost.

Two ICU invariants pull the artifact names in opposite directions, and the step satisfies both:

| Invariant | Value | Consequence |
|---|---|---|
| `U_ICUDATA_NAME` (what `udata` looks up) | `icudt76l` | the blob file and its ToC prefix must be `icudt76l` (`icupkg` forces prefix == basename) |
| `U_ICUDATA_ENTRY_POINT` (the linked symbol) | `icudt76_dat` | the genccode object must define `icudt76_dat` |

`genccode -n icudt76 -e icudt76 -f icudt76` writes `icudt76.c` defining `icudt76_dat` from the
blob named `icudt76l.dat`, so both hold. The CMake configure step rejects a pinned package whose
basename disagrees with `U_ICUDATA_NAME`, and the runtime proves the install by opening the root
bundle.

The step also writes the kept-locale manifest (`locales.json`: schema id, ICU version, data name,
blob bytes/items/SHA-256, sorted kept ids) and an equivalent compiled-in table
(`icu_locale_manifest.h`) so `LocaleResolver` needs no file I/O at runtime. Both are deterministic:
sorted ids, no timestamps, no host paths.

### 8.3 Measured cost on this lane

| Item | Value |
|---|---|
| Reduced blob | 17,293,904 B, 447 items, 22 locale bundles (54.3% of full) |
| Blob SHA-256 | `534d4de0…f9262f9f` |
| Data step (icupkg trim + manifest) | 1.0 s |
| genccode (54 MB of C) | 1.3 s |
| Data object compile | 15.4 s (one large TU) |
| ICU static libraries, cold, `-j4`, Debug | 11.5 s (455 translation units) |

### 8.4 Runtime, segmentation, and the manifest

`ui/text/icu_runtime.*` installs the blob with `udata_setCommonData` before any data call (lazily
from the segmentation/locale entry points, so a host does not have to order startup), asserts the
linked library is the pinned major version, and fails loudly if ICU cannot read the root bundle.

`ui/text/text_breaks.*` is the single break-opportunity API: `ubrk_*` word (UAX #29) and line
(UAX #14) iterators over a UTF-8 `UText` (so indices are native byte offsets), with the previous
reduced rules kept as the ICU-free backend and as the parity baseline. `word_left`/`word_right`
and the greedy wrap use it: `ui/text/text_wrap.cpp` places one UAX #14 segment per line unit
(LB18 keeps a whitespace run with the segment before it, and the wrap trims both segment edges),
so the whole wrap path is UAX #14, not only the over-wide-run split of the old word loop. The
existing wrap fixtures keep their output because their corpora are single-spaced ASCII; the
re-baselined cases and the measured fixture deltas are in `text.md` section 6. Intentional
differences (all in `tests/unit/icu/text_breaks_parity_tests.cpp`):

| Case | Builtin reduced rule | ICU |
|---|---|---|
| `don't` | three segments (`don`, `'`-separator, `t`) | one word (WB6/WB7) |
| `3.14` | split at the decimal point | one number (WB11/WB12) |
| Han run `你好世界` | one word | dictionary segmentation `你好 | 世界` (the bundled `cjdict`) |
| line breaks | after spaces/hyphens/CJK | full UAX #14 (superset of the builtin set) |
| wrap of `aaaa—bbbb` at 56 px | split at the byte budget: `[0,9)` / `[9,11)` | break after the em dash: `[0,7)` / `[7,11)` |
| Arabic | splits at the space | identical to builtin |

`ui/text/locale_resolver.*` normalizes a request (BCP-47 separators/case, script/region subtags),
validates it against the manifest, and returns `kept`, the fallback ancestor, the base direction
(from `uloc_getCharacterOrientation`), and a structured diagnostic:
`icu.locale.fallback requested=de_AT resolved=de` or `icu.locale.missing requested=zz`. That closes
the section 5 risk: `udat_open` returning `U_ZERO_ERROR` with root data can no longer pass silently.

### 8.5 Tests and the static-link proof

- `icu_locale_pack`: installs the blob, checks the manifest identity, opens kept and trimmed
  locales, golden-tests `unum` (`de` `1.234.567,5`, `en` `1,234,567.5`) and `udat` short-date
  patterns (`ja` `y/MM/dd`, `de` `dd.MM.yy`, root `y-MM-dd`), asserts `udat_open` succeeds for a
  trimmed locale, checks repeat-call determinism, and asserts that **no ICU dynamic library is
  loaded in the process** (`GetModuleHandleW` / `/proc/self/maps`), the ADR-012 property that the
  prebuilt import libraries could not provide.
- `text_breaks_parity`: the matrix above plus determinism, code-point-boundary checks, and the two
  prose corpora the existing wrap/paint fixtures pin (`one two three … ten`), including the
  post-space opportunity UAX #14 must offer for every interior whitespace run. The full wrap path is
  compared engine-to-engine: identical line boxes on the shared single-spaced corpus (40/80/120 px),
  the public `layout_text` pinned to the ICU engine, and the em dash difference above.
- `locale_resolver`: manifest membership/sorting, exact/fallback/missing resolutions and their
  diagnostics, normalization, and RTL/LTR direction for kept and trimmed locales.
- The release gate `scripts/check_static_link.py` (`dumpbin /dependents`, `ldd`, `otool -L`)
  remains the authoritative link-graph check for a packaged build.

### 8.6 The option flip

One switch, declared in `third_party/CMakeLists.txt`:

```powershell
cmake --preset dev -B build/lane-wrap-icu -DTESSERA_BUILD_VULKAN=OFF -DTESSERA_BUILD_POC=OFF `
  -DTESSERA_ICU_REDUCED_DATA=ON "-DTESSERA_ICU_TOOLS_DIR=<icu>/bin64" `
  "-DTESSERA_ICU_FULL_DATA=<icu>/icudt76l.dat"
```

`TESSERA_ICU_TOOLS_DIR` (pinned `icupkg` + `genccode`) and `TESSERA_ICU_FULL_DATA` (the pinned
`icudt76l.dat`) are required only when the switch is ON. `TESSERA_ICU_SOURCE_DIR` is optional
(pre-fetched sources). Nothing else in the tree branches on the option except `src/ui/text`, which
adds `icu_runtime.cpp`/`locale_resolver.cpp` and the ICU tests, and defines
`TESSERA_ICU_ENABLED` for consumers.

**Code side is ready.** With the greedy wrap on UAX #14 (`text.md` section 6) no call site is left
behind the option: the ON configuration builds with warnings-as-errors and
`ctest -R "text_|paint_text|paint_text_wrap|layout_|icu"` is 22/22 green (the 20 shared tests plus
`icu_locale_pack` and `text_breaks_parity`; `locale_resolver` passes too when run by name). What
keeps the default lanes OFF is provisioning and cache cost, not correctness.

Measured cost of the ON configuration (`build/lane-wrap-icu`, clang 22 Debug, Windows, `-j8`):

| Item | Value |
|---|---|
| Configure | 7.3 s + 1.2 s generate (OFF lane: 7.3 s + 1.0 s) |
| Clean rebuild of the ICU slice | 23.1 s wall, sccache warm (the lane's first 54 MB `icudt76.c` compile alone was 36.1 s) |
| Filtered text suite | 22 tests, 0.41 s (OFF lane: 20 tests, 0.39 s) |
| Static artifacts | `tessera_icu_common` 22.3 MB, `tessera_icu_i18n` 59.8 MB, `tessera_icu_data` 17.3 MB |
| Executable delta | `text_layout_tests.exe` 0.84 MB → 19.7 MB (the blob links into every `tessera_text` consumer) |

So the flip is roughly +25 s of cold build and +18 MB per binary on this lane, with ccache
absorbing the 455 ICU TUs after the first run. The cache burden is the real cost: the key set
grows by those TUs plus the generated 54 MB `icudt76.c`, and the two pinned host inputs
(`icupkg`, `genccode`) plus the 30.4 MB full `icudt76l.dat` are not in the repo, so every job that
flips the option must provision them.

CI flip lines: four lines in an existing Configure step (Linux shown; add
`-DTESSERA_ICU_SOURCE_DIR` only when the job pre-fetches the sources), preceded by one provisioning
step that places the pinned host tools and data at those paths:

```yaml
      - name: Configure
        run: |
          cmake --preset ci -DTESSERA_WARNINGS_AS_ERRORS=ON -DTESSERA_ENABLE_THORVG=ON \
            -DTESSERA_ICU_REDUCED_DATA=ON \
            "-DTESSERA_ICU_TOOLS_DIR=${{ runner.temp }}/icu/bin64" \
            "-DTESSERA_ICU_FULL_DATA=${{ runner.temp }}/icu/icudt76l.dat"
```

The filtered `ctest -R "text_|paint_text|paint_text_wrap|layout_|icu"` line already exists in the
test step, so the ICU tests join it without a workflow edit.

Fallback when enabling it on every job is too slow: keep every existing job on the builtin engine
and add one dedicated Linux `icu-text` job that carries the provisioning, the ON configure, the
build, and the filtered `ctest` lines above. That is what `linux.yml` does (timeout 40 minutes):
the job downloads the pinned `icu4c-76_1-src.tgz` (sha256-verified), builds its host tools with
`runConfigureICU Linux --disable-samples` plus `make`, and passes `TESSERA_ICU_TOOLS_DIR`,
`TESSERA_ICU_FULL_DATA` and `TESSERA_ICU_SOURCE_DIR` to the `ci` preset. The UAX #14 + ICU path is
then proven on every push for one job's build cost instead of every job's, and the parity test keeps
proving that the builtin engine the other lanes ship produces the same line boxes for the shared
corpus.

Making ICU mandatory later is: move the four option/set lines from `third_party/CMakeLists.txt` to
the root options block and default them ON.

