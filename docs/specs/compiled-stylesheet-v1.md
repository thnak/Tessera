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

## 3. Byte layout

All integers are little-endian and fixed width. Doubles are the IEEE 754 binary64 bit pattern.
Booleans are one byte, 0 or 1. Counts are `u32` and must not exceed the bytes remaining in the
payload (every element takes at least one byte), so a hostile count cannot drive an allocation.

```text
header (24 bytes)
  magic            4   "TSSB"
  format_version   u32 1
  engine_schema    u32 1
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
  nested selector list when set). Nesting depth is limited to 16.
- *Media query list*: `u32` queries; each `u8 negated`, type, `u32` conditions of
  (`u8 feature` 0..5, `u8 comparison` 0..2, value).
- *Container query*: name, `u32` conditions of (`u8 axis` 0..1, `u8 op` 0..4, `f64 value`).

## 5. Versioning and compatibility

- `format_version` changes when the byte layout changes. A loader rejects any version other than
  the one it was built with (`UnsupportedFormatVersion`); there is no forward reading.
- `engine_schema` changes when an engine change alters what prepared data means without changing the
  layout (new selector forms, different shorthand expansion, new supported at-rules). A mismatch is
  rejected (`EngineSchemaMismatch`). Every bump regenerates the committed blob goldens.
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

The theme asset section, the `AppHost` registration of several blobs and their import-graph order
are defined by ADR-048 and land in later slices; they will extend this document (a new section kind
needs a `format_version` bump).

## 9. Embedding in generated C++

`tessera_codegen` compiles the CSS it reads into blobs and passes them to codegen as opaque bytes
(`StylesheetEmbed{path, bytes}`); codegen never parses CSS, and the codegen library keeps its
IR-only dependency ([`ir-v1.md`](ir-v1.md), AGENTS.md non-negotiable 4). The CLI's bytes are exactly
`serialize_stylesheet(prepare_stylesheet(source))`; they are never re-encoded.

**When.** When at least one stylesheet of the module has a blob, `generate` appends exactly one
file to the component files it emits: `<module.name>_stylesheets.hpp`, next to `<Component>.hpp`.
The root namespace of the module is the C++ namespace of the file.

**Symbols.** One byte array per embedded sheet, then the record and the registration table:

```cpp
namespace <module.name> {
  inline constexpr std::array<std::uint8_t, N> kStylesheetBlob_<id> = { /* N bytes */ };
  struct EmbeddedStylesheet {
      std::string_view path;
      std::span<const std::uint8_t> bytes;
  };
  inline constexpr std::array<EmbeddedStylesheet, K> kEmbeddedStylesheets = {{
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

**Startup.** The app loads each table entry without parsing:

```cpp
for (const auto& sheet : <module.name>::kEmbeddedStylesheets) {
    const auto prepared = tessera::ui::style::load_stylesheet_blob(sheet.bytes);
    // a BlobError (format/engine schema, hash, corruption) is a build mismatch, not a CSS error
}
```

**Build-time CSS diagnostics.** The CLI compiles every read stylesheet before codegen runs. The
parser is tolerant by design (section 10 of [`stylesheet-v1.md`](stylesheet-v1.md)), so the
build-time failure it proves is a lexical one: an unterminated string is
`TESSERA_CSS_SYNTAX` with `path:line:column`, and an existing stylesheet the CLI cannot read is
`TESSERA_CSS_READ`. A missing file keeps the import's `TESSERA_IR_STYLESHEET_MISSING` error.
