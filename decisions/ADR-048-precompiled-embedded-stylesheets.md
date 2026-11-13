# ADR-048: Precompiled stylesheets embedded as versioned blobs, with a defined cascade order

- Status: Accepted (2026-11-13, owner)
- Date: 2026-11-12
- Resolves: the open question "build-time precompilation format: binary AST vs generated C++
  tables" in [`stylesheet-v1.md`](../docs/specs/stylesheet-v1.md) section 17.
- Related: [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (determinism),
  [ADR-014](ADR-014-project-config-format.md) (project config, packages),
  [ADR-016](ADR-016-stylesheet-engine.md) (stylesheet engine),
  [ADR-040](ADR-040-generated-base-and-developer-class.md) (generated code ownership),
  [ADR-046](ADR-046-web-target-for-demos.md) (web target),
  [ADR-047](ADR-047-component-library-layers-and-design-language.md) (component library, `::part()`),
  issues [#80](https://github.com/thnak/Tessera/issues/80),
  [#81](https://github.com/thnak/Tessera/issues/81) and
  [#78](https://github.com/thnak/Tessera/issues/78) (hot reload).
- Terms: **prepared stylesheet** = the media-independent compiled form defined in Decision item 2.
  **Blob** = the serialized bytes of one prepared stylesheet. **Import graph** = the graph of TSX
  modules and CSS files reachable from the app root through `import` statements.

## Context

[`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) section 4 lets a module write
`import "./App.css";`, and [`stylesheet-v1.md`](../docs/specs/stylesheet-v1.md) section 12 says
build-time CSS is precompiled to the runtime representation. Neither is implemented:

- the parser accepts the import, lowering skips it, `IrModule::stylesheets` is never filled and
  codegen emits nothing, so the stylesheet is silently dropped (issue #80);
- the only working path is a C++ raw string passed to `compile_stylesheet()` at startup
  (`samples/click-demo/scene.hpp`), so CSS errors surface at run time, every launch tokenizes and
  parses again, and the full parser ships in every release build;
- there is no rule for the cascade order of stylesheets from several files and packages, which
  matters as soon as `tessera-ui` ships CSS next to application CSS (ADR-047);
- the theme asset that section 9.1 says is embedded in the generated program has no embedding
  mechanism.

Two facts about the current engine shape the format:

1. **Media is evaluated during compilation.** `CompiledStyleSheet`'s constructor evaluates `@media`
   against the `MediaContext` it receives and drops non-matching rules. A build-time result that
   already applied media would be wrong as soon as the viewport, color scheme, reduced-motion or
   contrast preference changes.
2. **The parser AST is shallow.** `StyleSheet` keeps selectors, at-rule preludes and at-rule
   blocks as raw text that later stages parse again. Serializing that AST would save almost no
   startup work.

## Decision

1. **CSS is compiled at build time by the runtime's own engine.** Lowering resolves each
   side-effect CSS import (relative to the importing file, or through a package root from
   ADR-014), reads and hashes the file, and compiles it with the same tokenizer, parser, selector
   compiler and property registry the runtime links. Errors fail the build with file, line, column
   and the stylesheet diagnostic code; unsupported properties warn with the same codes as at run
   time. A missing file is an error at the import span.
2. **The engine gains a media-independent stage, the prepared stylesheet.** Compilation is split
   into:
   - **prepare** (build time, or run time for a CSS string): parse selectors into selector form,
     expand shorthands and validate values through the property registry, parse at-rule blocks
     recursively, parse `@media` and `@container` conditions into query trees **without
     evaluating them**, evaluate `@supports` (it depends only on the registry, which has the
     same version as the blob), parse keyframes, and fix layer order and source order;
   - **resolve** (run time): evaluate media conditions against the current `MediaContext` and
     build the cascade structures. Re-resolving after a media change needs no tokenizing or
     parsing.

   `compile_stylesheet(source)` stays as prepare + resolve for tests, tools and run-time CSS
   strings, and must produce the same result as loading the blob of the same source.
3. **A prepared stylesheet is serialized to a versioned binary blob.** Generated C++ tables are
   rejected (see Alternatives). The format:
   - a header with magic, format version, engine schema version and a content hash of the inputs;
   - little-endian fixed-width integers, offsets instead of pointers, an interned string table
     ordered by first use, no padding-dependent layout; the loader reads with `std::memcpy`, never
     by reinterpreting pointers;
   - byte-identical output for identical inputs and tool version (ADR-009);
   - the loader checks bounds and versions on every load, because blobs also arrive at run time
     through hot reload and the designer, and fails with a diagnostic instead of crashing.

   The format is specified in a new versioning spec, `compiled-stylesheet-v1.md`, before code
   lands, per the documentation rules for new artifacts.
4. **Embedding.** Codegen writes one generated source per app with the blobs as
   `inline constexpr` byte arrays (split into chunks to stay within compiler limits) and a
   registration table that `AppHost` loads at startup. Generated files stay build output under
   ADR-040. When the C++ baseline gains `#embed`, codegen may switch to it without changing the
   format. A blob whose format or engine schema version does not match the linked runtime is a
   build error; at run time it is rejected with a diagnostic.
5. **Cascade order across files and packages.** Stylesheets enter the cascade in import-graph
   order: depth-first from the app root module, imports visited in source order, each CSS file
   placed at its **first** occurrence (post-order, as web bundlers do). The resulting sheet order
   is the high part of `source_order` in the ordering key of `stylesheet-v1.md` section 7.2; the
   order inside a file is the low part. The order is recorded in the IR (`IrModule::stylesheets`)
   and shown by `tessera_codegen --explain-styles`.
6. **Library CSS lives in a cascade layer.** First-party library CSS is written inside
   `@layer tessera-ui`, and third-party packages are expected to use their own named layer.
   Unlayered application CSS beats layered CSS regardless of import order or specificity, so an
   app restyles library components with plain selectors and `::part()` (ADR-047 item 3) instead of
   specificity fights.
7. **The theme asset uses the same mechanism.** The JSON theme asset is validated at build time
   and embedded as a section of the app's style data, so theme switching (section 9) needs no file
   I/O or JSON parsing at run time.
8. **Hot reload and other hosts reuse the blob.** In dev mode a CSS edit produces a new blob that
   the devtools `reload` command (issue #78) swaps in; the designer and the web target (ADR-046)
   load the same bytes. No host has a separate CSS path.
9. **Run-time CSS strings remain allowed.** Tests, tools and apps that build CSS at run time keep
   using `compile_stylesheet()`. A release build that registers only blobs and never calls it does
   not need the tokenizer and parser; dropping them from such builds is measured with a link-size
   check, not assumed.

## Consequences

### Positive

- CSS errors are build errors with exact locations; an import can no longer be dropped silently.
- Startup loads prepared data instead of tokenizing and parsing; media and theme changes
  re-resolve without parsing.
- One format serves native, web, designer and hot reload.
- Editing CSS regenerates a data file, not large generated C++ tables, so incremental builds stay
  fast.
- Library and app CSS combine predictably through import-graph order and layers.

### Negative / risks

- The engine must be split into prepare and resolve, which touches `CompiledStyleSheet` and its
  tests.
- A new binary format must be versioned and kept compatible with the engine schema; every engine
  change that alters prepared data needs a version bump and regenerated goldens.
- Large generated byte arrays compile slowly on some compilers (MSVC in particular); chunking and,
  later, `#embed` mitigate it, and the build time is measured.
- Apps that relied on raw-string stylesheets keep working but do not get build-time diagnostics
  until they move to CSS imports.

## Alternatives considered

- **Generated C++ tables** (one `constexpr` structure per rule). Rejected: every CSS edit
  recompiles large generated C++, hot reload would need a C++ rebuild, the designer and web target
  would need a second path, and table layout would leak engine internals into generated code.
- **Serialize the current parser AST.** Rejected: selectors and at-rule blocks are raw text in that
  AST, so startup would still parse most of the stylesheet.
- **Serialize the fully compiled stylesheet with media applied.** Rejected: wrong after any
  viewport, color-scheme, motion or contrast change.
- **Embed the CSS source and parse it at startup.** Rejected: errors stay run-time only, startup
  keeps the parsing cost, and release builds always link the parser.
- **CSS modules (class names scoped per file) instead of layers.** Not chosen now: layers already
  exist in the engine and solve library-versus-app precedence; scoped class names can be added
  later without changing this format.

## Follow-up

- Fix issue #80 first: resolve CSS imports, fill `IrModule::stylesheets`, error on missing files,
  and warn while embedding is not implemented.
- Write `compiled-stylesheet-v1.md` (format, versioning, compatibility, loader validation).
- Split the engine into prepare and resolve; prove `compile_stylesheet(source)` equals
  load(serialize(prepare(source))) on the existing stylesheet test corpus.
- Serializer, loader with fuzzed bounds checks, codegen embedding and `AppHost` registration.
- Import-graph order in lowering, `--explain-styles`, `@layer tessera-ui` in
  `tessera/components` CSS.
- Theme asset embedding.
- Measurements: startup (blob load vs parse), build time of the generated arrays, release link
  size with and without the parser.
- Update `stylesheet-v1.md` sections 7.2, 9.1, 12 and 17 when this ADR is accepted.
