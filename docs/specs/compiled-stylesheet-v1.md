# Compiled stylesheet blob v1

Normative format of the precompiled stylesheet blob decided in
[ADR-048](../../decisions/ADR-048-precompiled-embedded-stylesheets.md). The CSS language itself is
specified in [`stylesheet-v1.md`](stylesheet-v1.md); this document only defines the serialized
form of a **prepared stylesheet** and its compatibility rules.

Implementation: `include/tessera/ui/style/prepared_stylesheet.hpp` (model and `prepare_stylesheet`),
`include/tessera/ui/style/stylesheet_blob.hpp` (`serialize_stylesheet`, `load_stylesheet_blob`),
`src/ui/style/stylesheet_blob_{write,read}.cpp`.

## 1. Pipeline

```text
CSS text --prepare--> PreparedStyleSheet --serialize--> blob bytes
blob bytes --load--> PreparedStyleSheet --resolve(MediaContext)--> CompiledStyleSheet
```

- **Prepare** is media independent: it parses selectors, expands shorthands and validates values
  through the property registry, parses at-rule blocks recursively, evaluates `@supports`, parses
  `@media` and `@container` conditions into query trees **without evaluating them**, parses
  keyframes and fixes layer and source order. Diagnostics found here (unsupported properties,
  invalid at-rules) are stored in the prepared sheet.
- **Resolve** is the `CompiledStyleSheet` constructor taking a `PreparedStyleSheet`: it evaluates
  `@media` against a `MediaContext`, assigns layer order and source order, and builds the selector
  index. It does not tokenize or parse. Resolving the same prepared data against another
  `MediaContext` is how media, color-scheme and reduced-motion changes are applied.
- `compile_stylesheet(source)` is prepare + resolve and must equal
  load(serialize(prepare(source))) resolved with the same `MediaContext`; the unit tests prove it on a
  corpus covering every item kind.
- **Startup cost.** `tests/benchmarks/stylesheet/` (CTest `stylesheet_startup_benchmark`, label
  `benchmark`, opt-in) runs one 21745-byte stylesheet through both paths with counters only: the
  blob path reads 35458 bytes and makes 2172 prepare allocations, while the parse path reads the
  same CSS, tokenizes 6047 tokens and makes 6660 prepare allocations for the same compiled sheet
  (218 rules, 239 selectors, 767 declarations, identical fingerprints), so the blob path saves the
  tokenizer, selector parser, shorthand expansion and `@supports` evaluation -- 4488 allocations and
  3.8 MB of allocation churn per startup -- at the cost of a 63 % larger payload (baseline:
  `tests/golden/perf/stylesheet_benchmark.json`).

## 2. Prepared model

A prepared stylesheet holds `source_hash`, a `layers` name table, a diagnostics list and `items` in
source order. Each item has a kind and the `@media` query lists enclosing it (it applies when all
match):

| Kind | Meaning |
| --- | --- |
| Rule | Parsed selector list, expanded declarations, layer index, enclosing `@container` queries |
| Layer | An `@layer` at-rule: the layers it names or creates; assigned an order on first applicable use |
| Container | An `@container` block's query, registered for the container-query tracker |
| Keyframes | A parsed `@keyframes` block |

Layer order follows the first **applicable** appearance (a layer that only appears inside a
non-matching `@media` takes no slot), exactly as the engine behaved before the split. Source order
is the running count of declarations of the applicable rules, so it is stable per media context.

The prepared model is the whole selector AST the cascade needs, so a selector that means something
to the engine is preserved exactly. A `::part()` rule is an ordinary rule here: its offset,
declarations, container queries and part names survive a `serialize_stylesheet` /
`load_stylesheet_blob` round trip and the reloaded sheet still styles the part
(`tests/unit/part_pseudo_tests.cpp`, case `blob_round_trip`).

## 3. Byte layout

All integers are little-endian and fixed width. Doubles are the IEEE 754 binary64 bit pattern.
Booleans are one byte, 0 or 1. Counts are `u32` and must not exceed the bytes remaining in the
payload (every element takes at least one byte), so a hostile count cannot drive an allocation.

```text
header (24 bytes)
  magic            4   "TSSB"
  format_version   u32 1
  engine_schema    u32 3
  source_hash      u64 FNV-1a 64 of the input CSS text (0 when prepared from a parsed tree)
  payload_size     u32 bytes after the header; must equal the remaining length exactly
payload
  string table     u32 count, then per string: u32 length, bytes (UTF-8)
  layers           u32 count, then a string index per layer
  diagnostics      u32 count, then per entry: u8 kind, u64 offset, string index property, string index message
  items            u32 count, then items
item
  u8 kind (0 Rule, 1 Layer, 2 Container, 3 Keyframes)
  u32 media count, then media query lists
  kind-specific body (section 4)
```

Strings elsewhere are `u32` indices into the string table. The table is ordered by **first use**
while writing the body, never by hash-map iteration, so equal input gives equal bytes.

## 4. Item bodies

- **Rule**: selector list, declarations, `u32 layer` (index into `layers` or `0xFFFFFFFF` for
  unlayered), `u32 count` then container queries.
- **Layer**: `u32 count` then `u32` layer indices.
- **Container**: one container query.
- **Keyframes**: name, `u32 count`, then per step `f64 offset` and declarations.

Shared structures:

- *Declaration*: property, value, `u8 important`, `u64 offset`.
- *Selector list*: `u32 count` of complex selectors; each is `u32 count` of compounds, the
  compounds, `u32 count` of combinators (`u8`, 0..3; the count must be compounds minus one).
- *Compound*: tag, id, classes (`u32` + strings), attributes (name, `u8 op` 0..6, value,
  `u8 case_insensitive`), pseudos (name, argument, `u8 pseudo_element`, `u8 has_nested`, then a
  nested selector list when set). Nesting depth is limited to 16. A `::part()` pseudo-element
  ([`stylesheet-v1.md`](stylesheet-v1.md) section 6.3) needs no new field: it is a pseudo with name
  `part`, `pseudo_element = 1`, and the whitespace-separated part names in `argument`, so the blob
  layout did not change for it.
- *Media query list*: `u32` queries; each `u8 negated`, type, `u32` conditions of
  (`u8 feature` 0..5, `u8 comparison` 0..2, value).
- *Container query*: name, `u32` conditions of (`u8 axis` 0..1, `u8 op` 0..4, `f64 value`).

## 5. Versioning and compatibility

- `format_version` changes when the byte layout changes. A loader rejects any version other than
  the one it was built with (`UnsupportedFormatVersion`); there is no forward reading.
- `engine_schema` changes when an engine change alters what prepared data means without changing the
  layout (new selector forms, different shorthand expansion, new supported at-rules). A mismatch is
  rejected (`EngineSchemaMismatch`). Every bump regenerates the committed blob goldens. It is **3**
  since the CSS Grid longhands landed (issue #249): `display: grid` and the `grid-*` declarations are
  kept and laid out as a grid, where an engine that wrote them under 2 either dropped them as
  unsupported or laid the container out as a flex container. It was **2** from `::part()` matching
  (issue #235): a blob carrying a `::part()` compound now styles a part element across the component
  boundary, which the engine that wrote it under 1 did not do.
- Blobs are build output tied to the tool version that wrote them. ADR-048 makes a mismatch a build
  error for embedded blobs; blobs that arrive at run time (hot reload, designer) are rejected with
  the diagnostic instead.

## 6. Loader validation

`load_stylesheet_blob` never reads outside the input and never throws. It checks: header length,
magic, both versions, the optional expected content hash (`HashMismatch`), the payload size, every
count against the remaining bytes, every enum range, every string and layer index, the combinator
count of each complex selector, selector nesting depth, and that no bytes remain after the last
item. Any failure returns a `BlobError` with a code (`Truncated`, `BadMagic`,
`UnsupportedFormatVersion`, `EngineSchemaMismatch`, `HashMismatch`, `Corrupt`) and a message. The
tests truncate at every length and corrupt every payload byte.

## 7. Determinism

For identical prepared data the output is byte-identical (ADR-009): fixed-width integers, no
padding, no pointers, no hash-map iteration, no timestamps or paths in the blob.

## 8. Not covered by v1

Ordered combination of several sheets and the `AppHost` registration API are section 10. The theme
asset is a separate artifact with its own magic and format version (section 11), so it does not
change this layout and does not need a `format_version` bump here.

## 9. Embedding in generated C++

`tessera_codegen` compiles the CSS it reads into blobs and passes them to codegen as opaque bytes
(`StylesheetEmbed{path, bytes}`); codegen never parses CSS, and the codegen library keeps its
IR-only dependency ([`ir-v1.md`](ir-v1.md), AGENTS.md non-negotiable 4). The CLI's bytes are exactly
`serialize_stylesheet(prepare_stylesheet(source))`; they are never re-encoded.

**When.** When at least one stylesheet of the module has a blob, `generate` appends exactly one
file to the component files it emits: `<module.name>_stylesheets.hpp`, next to `<Component>.hpp`.
The root namespace of the module is the C++ namespace of the file.

**Symbols.** One byte array per embedded sheet, then the registration table. The record type is
`tessera::ui::style::EmbeddedStylesheet` from
`include/tessera/ui/style/stylesheet_blob.hpp`, so the generated file includes that header and names
the type fully qualified (generated code is built into an app that links the engine):

```cpp
#include "tessera/ui/style/stylesheet_blob.hpp"

namespace <module.name> {
  inline constexpr std::array<std::uint8_t, N> kStylesheetBlob_<id> = { /* N bytes */ };
  inline constexpr std::array<tessera::ui::style::EmbeddedStylesheet, K> kEmbeddedStylesheets = {{
      { "<path>", kStylesheetBlob_<id> },
  }};
}
```

- **Identifiers.** `<id>` is `StylesheetRef::id` with every run of non-alphanumeric characters
  replaced by a single `_` (`css::ui/app` -> `css_ui_app`), so the symbol is a valid C++ identifier.
- **Ordering.** Arrays, table entries and table order follow `IrModule::stylesheets` order, which
  lowering fills sorted by path; the caller's embed order is irrelevant.
- **Bytes.** Array sizes are the blob sizes. Elements are lowercase `0xnn` hex, 12 per line, each
  line ending with a trailing comma, so the file is byte-for-byte deterministic for a given IR and
  blob set (ADR-009). `--check` covers the file like any other generated file.
- **A sheet without a blob** keeps its `TESSERA_CG_STYLESHEET_NOT_EMBEDDED` warning and is absent
  from both the arrays and the table.
- **An embed whose path is not a stylesheet of the module** is a codegen error,
  `TESSERA_CODEGEN_STYLESHEET_UNKNOWN`, naming the first unknown path in sorted order (independent
  of the caller's embed order).

**Startup.** The app hands the table to its host once, before the first frame; the host loads each
entry without parsing and combines them in table order (section 10):

```cpp
tessera::app::AppHost host(root, base_sheet, loop, config);
host.register_embedded_stylesheets(<module.name>::kEmbeddedStylesheets);
```

**Build-time CSS diagnostics.** The CLI compiles every read stylesheet before codegen runs. The
parser is tolerant by design (section 10 of [`stylesheet-v1.md`](stylesheet-v1.md)), so the
build-time failure it proves is a lexical one: an unterminated string is
`TESSERA_CSS_SYNTAX` with `path:line:column`, and an existing stylesheet the CLI cannot read is
`TESSERA_CSS_READ`. A missing file keeps the import's `TESSERA_IR_STYLESHEET_MISSING` error.

## 10. Ordered combination and `AppHost` registration

Several prepared sheets become one cascade in **import-graph order** (ADR-048 decisions 5-6). The
ordering key itself is [`stylesheet-v1.md`](stylesheet-v1.md) section 7.2; this section defines how
the per-file values are produced.

- **One global source order.** Rules are concatenated in sheet order and every declaration gets the
  next value of a single, monotonically increasing `source_order`; it never restarts at a sheet
  boundary. Equal origin, importance, layer and specificity are therefore broken by the later
  sheet, which is the web import rule.
- **Layer remapping.** Each sheet's flat layer indices are remapped into one global name table. A
  named layer is the same layer across sheets (`@layer base` in two files merges into one slot and
  takes the order of its first appearance); an anonymous layer keeps its position under a fresh
  unnamed slot. `CompiledStyleSheet(std::span<const StylesheetSource>)` combines library sources
  before app sources, so every library layer sorts below every app layer; `append_sheet()` is the
  incremental form the host uses.
- **`@layer tessera-ui` for library sheets.** A `StylesheetSource` with `library = true` puts every
  rule of that sheet in `@layer tessera-ui`: its unlayered rules land in `tessera-ui` itself and its
  named layers nest under it (`@layer base` -> `tessera-ui.base`). Unlayered app CSS therefore beats
  library defaults regardless of specificity or import order, and library CSS only orders itself
  against earlier library layers. A library sheet that already writes its CSS inside
  `@layer tessera-ui` keeps that name instead of nesting it twice.
- **Keyframes, containers, diagnostics.** The first definition of a keyframes name wins across
  sheets; later definitions of that name are dropped, new names append in sheet order. Container
  queries and diagnostics concatenate in sheet order.
- **`AppHost`.** `AppHost::register_embedded_stylesheets(std::span<const EmbeddedStylesheet>)` loads
  each blob with `load_stylesheet_blob` — no CSS parsing — and combines it with the host's
  constructor sheet, which keeps the lowest source order, in table order. It replaces the host's
  compiled sheet and marks style, layout, paint and render dirty, so the next `begin_frame()`
  rebuilds the selector tree; `begin_frame()` itself never parses CSS. The host keeps the
  constructor sheet, so calling the method again replaces the previous registration instead of
  stacking on it. Library CSS reaches the host already layered: construct the constructor sheet
  from `StylesheetSource` with `library = true`.
- **A blob that does not load** (format version, engine schema, hash or corruption) is never applied
  and never throws. The sheet is skipped, the remaining sheets still register, one
  `TESSERA_CSS_BLOB_INVALID: <path>: <code>: <message>` line is written to stderr, and
  `AppHost::rejected_stylesheet_count()` reports how many were skipped. The cascade is always built
  from the sheets that did load plus the constructor sheet, never a partly applied sheet and never
  a previous registration.

## 11. Theme asset blob

The embedded form of the **theme asset** ([`stylesheet-v1.md`](stylesheet-v1.md) section 9.1).
Implementation: `include/tessera/ui/style/theme_asset.hpp` (`ThemeAsset`, `parse_theme_asset`,
`serialize_theme_asset`, `load_theme_asset_blob`), `src/ui/style/theme_asset.cpp` (build-time
parse), `src/ui/style/theme_asset_blob.cpp` (blob), `src/ui/style/theme_stylesheet.cpp` (the
generated `:root` rule).

```text
JSON asset --parse (build time)--> ThemeAsset --serialize--> blob bytes
blob bytes --load (startup)--> ThemeAsset --prepare_theme_stylesheet--> PreparedStyleSheet
```

The runtime never parses the asset JSON: the CLI parses it once at build time, and startup only
decodes the blob and appends the active theme's `:root` rule to the cascade.

### 11.1 Byte layout

All integers are little-endian and fixed width. Strings are `u32` length followed by UTF-8 bytes
(no interned table). Every count and length is bounds-checked against the bytes remaining before it
can drive a read or an allocation.

```text
header (20 bytes)
  magic            4   "TTHB"
  format_version   u32 2
  payload_hash     u64 FNV-1a 64 of every payload byte
  payload_size     u32 bytes after the header; must equal the remaining length exactly
payload
  schema_version   u32 the asset's `schemaVersion` (1)
  default_authored u8  1 when the asset carries a `themes.default` entry
  start            string
  warnings         u32 count, then a string per TESSERA_THEME_TOKEN_MISSING finding
  themes           u32 count, then per theme:
                     id      string
                     version string (the theme's `themeVersions` entry, empty when unversioned)
                     tokens  u32 count, then per token: name string, value string
```

`themes` is ordered: the built-in `default` first (its tokens replaced by an authored
`themes.default`), then the `themes` keys in document order. `warnings` is the parse-time derived
list, carried in the blob so the loaded asset equals the parsed one without re-deriving it at run
time.

### 11.2 Versioning and determinism

- `format_version` changes when the byte layout changes; the loader rejects any other value
  (`UnsupportedFormatVersion`) and never reads forward.
- Format 2 added the per-theme `version` string; a format 1 blob is `UnsupportedFormatVersion`
  (there is no forward or backward reading). A non-empty `version` that is not
  `MAJOR.MINOR.PATCH` is `Corrupt`.
- `payload_hash` is the hash the writer computed over the payload; a mismatch is `HashMismatch`.
  It detects truncation or a corrupted/edited payload, not a deliberate forgery.
- A valid hash with a payload that still fails the structural checks (unsupported `schema_version`,
  a count/length beyond the payload, trailing bytes) is `Corrupt`; a short header or a payload size
  that does not match the input is `Truncated`; wrong magic is `BadMagic`.
- Identical assets serialize to identical bytes: fixed-width integers, document/asset order for
  themes and tokens, no hash-map iteration, no paths or timestamps. `serialize(load(blob)) == blob`.
- A format/engine mismatch is a build error for an embedded blob (ADR-048); a blob that arrives at
  run time is rejected with the diagnostic.

### 11.3 Codegen emission

The CLI reads `assets.theme` from `tessera.json` (a project-relative POSIX path), parses it with
the harness JSON decoder, and passes the serializer output to `codegen::generate` as an optional
`EmbeddedTheme{bytes}`; the IR is unchanged and the codegen library still links only the IR. A parse
failure fails the regeneration with `<path>: <typed code>: <message>` (the codes of
[`stylesheet-v1.md`](stylesheet-v1.md) section 9.1) and writes no theme file. Without
`assets.theme` there is no extra file and no new warning.

When a theme is present, `generate` appends exactly one file, `<module.name>_theme.hpp`:

```cpp
#include "tessera/ui/style/theme_asset.hpp"

namespace <module.name> {
  inline constexpr std::array<std::uint8_t, N> kThemeBlob = { /* N bytes */ };
  inline constexpr tessera::ui::style::EmbeddedTheme kEmbeddedTheme = {
      std::span<const std::uint8_t>(kThemeBlob)};
}
```

Elements are lowercase `0xnn`, 12 per line, trailing comma per line, so the file is byte-for-byte
deterministic; `--check` covers it like any other generated file. With embedded stylesheets too,
`<module>_stylesheets.hpp` comes first and `<module>_theme.hpp` last.

### 11.4 `AppHost` registration and switching

```cpp
tessera::app::AppHost host(root, base_sheet, loop, config);
host.register_embedded_theme(<module.name>::kEmbeddedTheme.bytes);
host.set_theme("dark");                           // false for an unknown id
```

- `register_embedded_theme` loads the blob with `load_theme_asset_blob` (no JSON parsing), stores
  the asset, and applies its `start` theme. Calling it again replaces the previous asset.
- `set_theme(id)` activates an id of the installed asset. Only that theme's tokens are injected, as
  one generated `:root` rule with `Origin::Theme` appended to the ordinary cascade; there is no
  parallel token path. The host rebuilds the selector tree, marks the subtree
  `Style | Paint | Render` dirty, drops the pipeline's paint cache, and wakes the runtime loop, so
  the next `begin_frame()` re-resolves. A token the active theme omits resolves through `var()`
  fallback; a reserved chrome token additionally falls back to its built-in value (section 9.2 of
  [`stylesheet-v1.md`](stylesheet-v1.md)).
- `theme_ids()` returns `default` plus the asset's theme keys in asset order; `active_theme()`
  returns the active id; `theme_token(name)` returns the value the active theme injects (including
  the built-in reserved fallback) or nullopt.
- A blob that does not load is never applied and never throws: one
  `TESSERA_THEME_BLOB_INVALID: <code>: <message>` line goes to stderr, the current theme stays
  applied, and `rejected_theme_count()` reports the skip. A later loadable registration clears the
  count.
- The two reserved chrome tokens are also read outside the render tree
  ([`window-chrome-v1.md`](window-chrome-v1.md) section 3.1). This blob only carries the tokens the
  style engine injects; resolving the chrome pair before the first frame is the chrome slice's
  reader of the same asset.
