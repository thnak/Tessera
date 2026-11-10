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
[`text.md`](text.md) section 13. Follow-up implementation slice:

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
