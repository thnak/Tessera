# Studio services v1

Status: proposed (V1-M4); the protocol core, its no-UI harness and the `analysis` and `format` workers are implemented
(`designer/web/services/`, `scripts/ci/preview_studio_services.mjs`). Decision:
[ADR-060](../../decisions/ADR-060-studio-services-in-web-workers.md). Related:
[designer-protocol-v1.md](designer-protocol-v1.md) section 2.1 (transport),
[designer-studio-v1.md](designer-studio-v1.md) sections 2.2 and 4,
[studio-mock-services-v1.md](studio-mock-services-v1.md) (the mock worker handle),
[mcp-server-v1.md](mcp-server-v1.md).

## 1. Scope

Every studio service (`viewer`, `analysis`, `format`, `mock`) is a Wasm module in its own Web Worker
and speaks the protocol below. The protocol is plain data over `postMessage`: nothing in it names
React, the DOM or CodeMirror, so the page of today and the Tessera-built studio of V1 use the same
workers. This document defines the envelope, versioning, cancellation, text ownership, capabilities
and restart. It does not define the language requests themselves; each of those issues adds its request and response types
to section 8 and a section of its own.

## 2. Plain data

A message is structured-cloneable plain data: null, boolean, string, finite number, array, plain
object, `ArrayBuffer` and typed-array views. A function, symbol, bigint, `undefined`, a class instance
(a DOM node, a `Map`) or a cycle is invalid, and the sender's schema check refuses it before
`postMessage` (error code `TESSERA_STUDIO_MALFORMED`). Nesting is limited to 32 levels.

| Type | Encoding |
|---|---|
| File id | non-negative integer assigned by the page for the open project; a path is never a key |
| Position | `{ file, offset }`, `offset` in UTF-16 code units of the file text (the unit of a JavaScript string and of CodeMirror) |
| Span | `{ file, from, to }`, two offsets, `to` exclusive |
| TextEdit | `{ file, offset, length, newText }` as [designer-protocol-v1.md](designer-protocol-v1.md) section 3.3, with UTF-16 `offset` and `length` here |

The viewer protocol keeps UTF-8 byte offsets (its section 3.1). A client converts at the boundary of
the viewer; the services in this document never see byte offsets. The conversion is
`designer/web/src/lib/utf8-edits.ts`.

## 3. Envelope

```json
{ "v": 1, "kind": "request", "service": "analysis", "type": "hover", "id": 17, "version": 42, "payload": {} }
```

| Field | Meaning |
|---|---|
| `v` | Protocol major version, `1` |
| `kind` | `request`, `response` or `event` |
| `service` | The service name from section 6 |
| `type` | The request name; a response repeats it |
| `id` | Request id, an integer unique per client; a response repeats it. Absent on events |
| `version` | The project version (section 4). A request carries the version it was made against; a response carries the version it answers. Absent on events |
| `payload` | An object. A response payload is `{ "result": ... }` or `{ "error": { "code", "message" } }` |

A receiver answers an unknown `type` with an error response `TESSERA_STUDIO_UNKNOWN_TYPE` and
continues. Messages from one sender are processed in order. A worker never blocks the page: every
request is answered asynchronously, and long work yields between files.

## 4. The page owns the text

The page holds the files and the **project version**, an integer it increases on every text change.
A worker holds only a cache keyed by that version.

- `snapshot`: `{ files: [{ file, bytes }] }` where `bytes` is a UTF-8 `ArrayBuffer` in the transfer
  list. It replaces the whole cache and sets the cached version to the request's `version`.
- `delta`: `{ baseVersion, edits: [TextEdit] }`. The edits apply one after another, each against the text
  the previous one produced (an edit list in the coordinates of one text goes through
  `toSequentialEdits` first). It is applied only when `baseVersion` equals the
  cached version, and sets the cached version to the request's `version`. Otherwise the worker
  answers `TESSERA_STUDIO_VERSION` and the page sends a snapshot.
- Any other request before the first snapshot is answered `TESSERA_STUDIO_NO_SNAPSHOT`. A request
  whose `version` is newer than the cache is answered `TESSERA_STUDIO_VERSION`.

## 5. Cancellation and stale answers

- **Latest wins.** A client sends a request of a type while an earlier request of that type is still
  open: the earlier one is cancelled. The client rejects it with `TESSERA_STUDIO_CANCELLED` and sends
  `cancel` (`{ target: id }`) so the worker can stop; a worker may still answer, and the client
  ignores an answer to a request it no longer has open.
- **Stale answers are dropped.** When a response's `version` is older than the page's current
  version, the client does not deliver it; it rejects with `TESSERA_STUDIO_STALE` and the caller
  re-asks if it still needs the answer. `snapshot`, `delta` and the handshake answer for the version
  they set and are exempt.
- A pool (ADR-060 section 1) routes by file id; whole-project requests fan out one file per worker and
  the client merges the answers that carry the current version.

## 6. Capabilities

The first request to every worker is `capabilities` (empty payload). The result is
`{ name, protocol, features }`: the service name, the highest protocol major version it speaks, and a
list of feature names (request types it implements). The client uses it to tell a missing feature
from a failure: it does not send a request type the worker did not list. The MCP server reuses the
same requests and capability names ([mcp-server-v1.md](mcp-server-v1.md)).

## 7. Restart

A worker that crashes or is restarted loses only its cache. The client starts a new worker, repeats
the handshake and sends a `snapshot` of the current text; open requests are rejected with
`TESSERA_STUDIO_CANCELLED`, so a crash costs those requests and no state.

## 8. Services

| Service | Features at this version |
|---|---|
| `viewer` | Wraps the viewer protocol of [designer-protocol-v1.md](designer-protocol-v1.md); not yet moved behind this envelope |
| `analysis` | `symbols` (section 10, #570), `diagnostics` (section 11, #568), `definition`, `references` and `hover` (section 12, #571, #572); completions and rename are the analysis issues that follow |
| `format` | Feature `format`: request `{ file, printWidth?, indentWidth? }`, result `{ file, changed, edits: [TextEdit], diagnostics }`. The edits are the minimal list of [source-format-v1.md](source-format-v1.md) section 11, in the coordinates of the snapshot version answered. A refusal is a result (no edits, the diagnostics say why), not an error. Errors: `TESSERA_FMT_NO_FILE` (id not in the snapshot), `TESSERA_FMT_ENGINE` (the module failed) |
| `mock` | The mock worker of ADR-055 keeps its own handle; it adopts the envelope when it is next changed |

Error codes: `TESSERA_STUDIO_MALFORMED`, `_VERSION`, `_NO_SNAPSHOT`, `_UNKNOWN_TYPE`, `_CANCELLED`,
`_STALE` (the last two are client-side outcomes and never travel as a worker answer).

## 9. Harness

`designer/web/services/protocol.js` holds the schema check, `client.js` the page-side client
(versions, latest wins, stale drop, handshake, restart) and `stub-service.js` a stub worker. The gate
`scripts/ci/preview_studio_services.mjs` runs them under Node with `worker_threads`, with no UI, and
checks the handshake, versioned answers, cancellation, the stale drop, restart from a snapshot and
the refusal of a function or DOM-like value. The worker handle is the one of
[studio-mock-services-v1.md](studio-mock-services-v1.md) (`post`, `onMessage`, `onError`,
`terminate`), so the same client drives a blob `Worker` in a browser. Real services reuse the gate by
swapping the stub's source: `scripts/ci/preview_format_service.mjs` does it for the `format` worker
(`designer/web/services/format-service.js`, built around an engine; the page's engine is
`tessera_format.wasm`). With `TESSERA_FMT_BIN` set to a built `tessera_fmt` the gate also runs every case
against the real printer.


## 10. Symbol and span index (`symbols`)

The index of declarations and references that definition, references, hover, completions and rename
(#571 to #574) answer from. The library is `src/compiler/analysis` (`tessera_analysis`), portable C++
over the front end of [ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md), so the Wasm worker, the
CLI and the MCP server share it. Feature name: `symbols`, listed by the worker's `capabilities`
(`analysis_capabilities_json()`). The worker host is section 11.

**Request** `symbols`: payload `{ file? }`, one file id or every file. **Result**
`{ files: [{ file, indexed, symbols, references }] }`, in file id order. `indexed` is false for a file
that is not `.ts`/`.tsx` or that the parser could not read.

| Record | Fields |
|---|---|
| symbol | `id`, `kind`, `name`, `from`, `to` (the declared name), `parent` (enclosing component or hook id, empty at module scope), `exported`, and for `import`/`cssModule` a `target` |
| reference | `kind`, `name`, `from`, `to`, and either `symbol` (the declaration id) or `unresolved` (the reason) |

- **Spans** are the `Span` of section 2: UTF-16 code units of the file text with the file id, `to`
  exclusive. The text is the decoded text without a leading byte order mark (what a `TextDecoder`
  and CodeMirror hold); the library converts from the parser's UTF-8 byte offsets and counts an
  astral code point as two units and an invalid byte as one.
- **Symbol kinds**: `component`, `hook`, `helper`, `type`, `const`, `import`, `cssModule`, `prop`,
  `propsParam`, `state`, `setter`, `handler`, `local`, `param`.
- **Stable ids**: `f<file>:` plus the scope path (`Counter/count`; `Counter/~1/e` is the parameter of the
  first anonymous function of `Counter`), with `#n` for the n-th duplicate in a scope. An id holds no
  offset, so an edit elsewhere in the file leaves it unchanged; renaming or moving the declaration
  changes it.
- **Reference kinds**: `value`, `type`, `tag` (opening and closing name), `attribute`, `cssClass`,
  `member`, `export`.
- **No silent drops.** A reference resolves to a declaration or carries one reason: `unbound` (nothing in
  scope: a library component, a typo), `builtin` (a builtin type, hook or global), `intrinsic` (a
  lowercase tag and its attributes), `external` (a CSS class, declared in the `.css` file the index
  does not read), `needsType` (a member access or an attribute the component does not destructure),
  `importUnresolved` (the file or exported name is not in the snapshot).
- **Cross-file resolution.** A relative import specifier resolves against the file paths of the
  snapshot (`.tsx`, `.ts`, `/index.*`); the import symbol's `target` is the exported declaration,
  following `export type { A } from` chains. An attribute on a component tag resolves to that
  component's destructured `prop`, through imports. Package specifiers are `external`.
- **Not indexed**: the body of a `helper` (the parser skips it), CSS class declarations, object
  literal keys, and the property of a member access (it needs the receiver's type, `needsType`).
- **Determinism and incremental update.** Each file's index is a pure function of its text, and
  resolution of the paths and those indexes, so `delta` edits (applied in order, each against the text the
  previous one produced) reindex only the edited files and re-resolve imports, and the result equals a
  fresh build of the same files; the tests compare the two byte for byte. Output order does not depend
  on the order the files were given.

Tests: `tests/unit/analysis_index_tests.cpp` (declarations, cross-file, UTF-16 and BOM offsets,
incremental equals fresh, and span/resolution invariants over `samples/` and
`tests/fixtures/corpus/`). The Wasm module, worker and budget are section 11.

## 11. Analysis worker and project diagnostics (`diagnostics`)

[ADR-060](../../decisions/ADR-060-studio-services-in-web-workers.md) section 1: the `analysis` service is
`tessera_analysis.wasm` (`createTesseraAnalysis`, target `tessera_analysis_wasm`, linked from
`src/compiler/analysis`), loaded lazily by `designer/web/services/analysis-service.js` in its own worker on the first
request. The page-side client and the gate are the generic ones of section 9.

- **Snapshot paths.** A `snapshot` file entry may carry `path`, the project-relative path with `/`: it is how a
  relative import resolves (section 10) and what a duplicate-component message names. A file with no path
  resolves no imports. `ServiceClient.setSnapshot` takes `[id, text, path?]`.
- **Request** `diagnostics`: `{ file? }`. **Result** `{ files: [{ file, diagnostics: [Diagnostic] }] }`, one entry
  per file of the project (or only `file`) in file id order, an empty list for a clean file. A `Diagnostic` is
  `{ code, severity: "error" | "warning", message, from, to }` with the UTF-16 `Span` of section 2 (file implied by
  the entry) and, for a finding about two places, `related: { file, from, to, message }`. Order within a file is
  `from`, `to`, `code`, `message`, so it does not depend on the order the files were given.
- **What is reported.** (1) The parser's diagnostics of each `.ts`/`.tsx` file, `TESSERA_JSX_*`, with their codes;
  parsing recovers per top-level item, so one syntax error costs that item and the file's other items still
  index. (2) `TESSERA_ANALYSIS_IMPORT_FILE`: a relative import no file of the snapshot matches (error; a missing
  `.module.css` is a warning, because a snapshot may carry only the sources). (3) `TESSERA_ANALYSIS_IMPORT_NAME`:
  the file exists but does not export the name; not reported when that file has parse errors, since its own
  diagnostics are the cause. (4) `TESSERA_ANALYSIS_DUPLICATE_COMPONENT`: a component name declared at module scope
  more than once in the project; every declaration is reported, with `related` pointing at another one. An error in
  an imported file is reported against that file, never against its importer. The semantic analyzer and lowering
  diagnostics (`TESSERA_REACT_*`, `TESSERA_IR_*`) are not run here yet: they need a whole-project lowering pass.
- **Engine ABI.** `src/compiler/analysis/src/analysis_entry.h`: stage files with `tessera_analysis_set_file` and
  `tessera_analysis_commit_snapshot`, edits with `tessera_analysis_edit` and `tessera_analysis_commit_edits` (0 when an
  edit names an unknown file or a range outside it; the worker then answers `TESSERA_STUDIO_VERSION`), and ask with
  `tessera_analysis_query_json("symbols" | "diagnostics" | "capabilities", file or -1)`. The same functions link
  natively into `tests/unit/analysis_diagnostics_tests.cpp`, which compares them with the library byte for byte.
- **Worker rules.** Changes (`snapshot`, `delta`) and queries run in arrival order, so an answer is computed
  against the version it carries; a query answered for an older version is dropped by the client
  (`TESSERA_STUDIO_STALE`). Errors: `TESSERA_ANALYSIS_NO_FILE`, `TESSERA_ANALYSIS_ENGINE`. Capabilities:
  `{ name: "analysis", protocol: 1, features: ["symbols", "diagnostics"] }`.
- **Gate.** `scripts/ci/preview_analysis_service.mjs` (workflow step in `pages.yml`): protocol cases against a JS
  stand-in engine (latest wins, an edit while a request is in flight drops the stale answer, a 150 ms parse in the
  worker leaves the page thread's longest gap under 100 ms, a crashed engine is an error response) and, with
  `TESSERA_ANALYSIS_WASM` set to a built `tessera_analysis.js`, the same cases and the project findings against the
  C++ library.
- **Budget.** The module is lazy, so it is not in `download_gzip_bytes`; `lazy_service_modules_gzip_bytes` in
  `tests/golden/perf/preview_budget.json` weighs it with the format module (`scripts/preview_budget_measure.py`,
  from the assembled `build/pages`).

## 12. Definition, references and hover (`definition`, `references`, `hover`)

Position requests of the `analysis` service (#571, #572), answered from the index of section 10 by
`src/compiler/analysis/include/tessera/compiler/analysis/language.hpp`. Feature names `definition`, `references` and
`hover`. Every request carries `{ file, offset }`: a file id (required, `TESSERA_ANALYSIS_NO_FILE` when it is not in the
snapshot) and a non-negative integer UTF-16 `offset` of the caret (`TESSERA_STUDIO_MALFORMED` otherwise). A caret at the end
of a name still names it; a name that starts at the caret wins. An offset past the end or on no name is an empty
answer, not an error. Spans are the `Span` of section 2 with the file id. As for every request, a response older than
the page's version is dropped by the client (`TESSERA_STUDIO_STALE`), so a hover that arrives after an edit never shows.

- **`definition`**: result `{ definitions: [{ file, from, to }], unresolved? }`. The span is the declared name. An import
  binding (and every use of one) is followed through imports and `export { X } from` chains to the declaration, so
  F12 on `<Card />` lands in the file that declares `Card`, and an attribute on a component tag lands on that
  component's destructured prop. An import that does not resolve (a package, a missing file or name) answers with the
  import binding itself. When there is no declaration `definitions` is empty and `unresolved` carries the reason of
  section 10 (`external` for a CSS class, `builtin`, `intrinsic`, `needsType`, `unbound`); on no name at all it is
  absent. There is at most one definition today; the list leaves room for overloads.
- **`references`**: payload adds `includeDeclaration` (boolean, default true). Result `{ symbol, name?, references:
  [{ file, from, to, kind }], unresolved? }`. `symbol` is the id of the declaration the caret resolves to (imports
  followed, so the caret on a use, an import binding or the declaration gives the same list), `null` when there is
  none. Each entry is a use of the declaration directly or through an import; `kind` is the reference kind of section 10,
  or `declaration` for the declared name and for the import bindings that stand for it (omitted when
  `includeDeclaration` is false). Sorted by file id, then offset.
- **`hover`**: result `{ hover: null | { from, to, kind, name, signature?, doc?, container?, exported?, declaration?,
  unresolved?, diagnostics } }`. `from`/`to` is the name under the caret (the tooltip's anchor; for a diagnostic with
  no name there, the diagnostic's span). `kind` is the declaration's symbol kind, or the reference kind of an
  unresolved name (`cssClass`, `tag`, ...) with `unresolved` giving the reason. For a resolved name: `signature` is
  the first line of the declaring statement with a function body cut off at its `{` (at most 200 characters; empty for
  props and parameters, whose types the index does not keep), `doc` the contiguous `//` or `/** */` comment lines
  directly above that statement with the markers removed, `container` the enclosing component or hook name, and
  `declaration` the span of the declared name, possibly in another file. `diagnostics` lists the project diagnostics of
  section 11 of the file that cover the caret (`{ code, severity, message, from, to }`; an empty span covers only its
  own offset). `{ hover: null }` when the caret names nothing and no diagnostic covers it. Type information beyond the
  signature text is not available until the type checker feeds the index.
- **Engine ABI.** `tessera_analysis_request_json(type, file, offset, flag)` in `analysis_entry.h` (`flag` is
  `includeDeclaration`); null for an unknown type or a negative file or offset, which the worker reports as
  `TESSERA_ANALYSIS_ENGINE`. The engine function of the worker is `tesseraAnalysisQuery(type, file, offset, flag)`.
  Capabilities: `features: ["symbols", "diagnostics", "definition", "references", "hover"]` (section 13 adds `completion`, `prepareRename`, `rename`).
- **Tests and gate.** `tests/unit/analysis_language_tests.cpp` (cross-file definition and references through imports,
  props and CSS classes, UTF-16 offsets, hover signature, doc and diagnostics, edits, ABI parity);
  `scripts/ci/preview_analysis_service.mjs` (request validation, argument passing and stale drop against the stand-in
  engine and, with `TESSERA_ANALYSIS_WASM`, the same requests against the C++ library). The editor wiring (F12,
  Shift+F12, tooltip, the `go.*` commands gated on `service.analysis.ready`) and its Playwright cases are separate studio
  work.

## 13. Completions and rename (`completion`, `prepareRename`, `rename`)

Position requests of the `analysis` service (#573, #574) with the validation of section 12 (`{ file, offset }`);
the engine is `completion.hpp` and `rename.hpp` beside `language.hpp`. Capabilities add `completion`, `prepareRename`
and `rename`. `completion` and `prepareRename` use `tessera_analysis_request_json`; `rename` carries `newName` (a string,
`TESSERA_STUDIO_MALFORMED` otherwise) and uses `tessera_analysis_rename_json(file, offset, newName)`; the engine function
takes it as a fifth argument.

- **`completion`**: result `{ from, to, context, items: [{ label, kind, detail? }], truncated }`. `[from, to)` is the
  word at the caret (UTF-16; for `importPath` the string content), which the chosen label replaces. The list is a function
  of the project version and the caret only: exact-case prefix matches first, then label order, no duplicates, at most
  200 items (`truncated`). The editor filters further as the user types; suggested trigger characters are `<`, `.`, `/`,
  `"`, `:` and a space after a tag name. `context` and what each offers:

  | `context` | Caret | Items (`kind`) |
  |---|---|---|
  | `tag` | after `<` or `</` | components in scope: declared (`component`) and imported (`import`, `detail` the source) |
  | `attribute` | inside an open tag, not after `=` | `key`, the destructured props of the tag's component through imports (`prop`), `className` on a lowercase tag; attributes already written are left out |
  | `cssClass` | after `styles.` on a CSS module binding | class selectors of the `.css` file when it is in the snapshot (`cssClass`) |
  | `member` | after `.` of anything else | none (needs the receiver's type) |
  | `importPath` | inside the string of `import`/`export ... from` | project files as relative specifiers without `.tsx`/`.ts` (`file`) |
  | `bind` | after `bind::` in a `//` comment | `bind::` ids already used in the project and the snake_case names of the component's handlers (`bind`) |
  | `expression` | anywhere else in code | names in scope, innermost first: state, setters, handlers, props, locals, components, hooks (symbol kinds) |
  | `none` | in a string or a comment | none |

  Scope is module scope plus the component or hook of the last module-level declaration at or before the caret (the index
  keeps no body ranges); a parameter of an anonymous function is not offered. CSS class names are read from the
  style sheet text by a small selector scanner (selector preludes only, comments, strings and declaration values skipped).
  A top-level item with a syntax error contributes no names, so an unfinished `<Ca` still completes from the module scope
  and imports but not from that item's locals. Not offered yet: auto-import of a component from another file, intrinsic
  tag names, the members of a value.
- **`prepareRename`**: result `{ prepare: { from, to, placeholder, kind } }` for a renameable name (a use, an import binding
  or the declaration; `kind` is the symbol kind or `cssClass`), else `{ prepare: null, refusal: { code, message } }`.
- **`rename`**: result `{ edits: [{ file, offset, length, newText }], refusal? }`. The edits are the `delta` edits of
  section 4, non-overlapping, sorted by file id and offset descending, so applying them in order and applying them all against
  the original text agree. Renaming from a use, an import binding or the declaration gives the same edits; the declaration,
  every import binding, `export { X }` entry, tag, attribute and use in every file change. A CSS class renames its selectors in
  the module's `.css` file and every `styles.x` use through every binding of that file. A refusal carries empty `edits`;
  nothing is half-renamed. Codes:

  | Code | Refused because |
  |---|---|
  | `TESSERA_ANALYSIS_RENAME_NO_NAME` | the caret names nothing |
  | `TESSERA_ANALYSIS_RENAME_UNRESOLVED` | no declaration: builtin, intrinsic tag, unbound name, member access |
  | `TESSERA_ANALYSIS_RENAME_EXTERNAL` | declared outside the project: a package import, a missing file or name, a style sheet or class not in the snapshot |
  | `TESSERA_ANALYSIS_RENAME_GENERATED` | an edit would land in generated code: a `gen`, `generated` or `__generated__` directory, a `.gen.`/`.generated.` file name, or an `@generated`/`DO NOT EDIT` header in the first 512 bytes |
  | `TESSERA_ANALYSIS_RENAME_ALIAS` | a binding is spelled differently (`import { A as B }`, `{ a: b }`) |
  | `TESSERA_ANALYSIS_RENAME_INVALID_NAME` | not an ASCII identifier (a CSS class: letters, digits, `_`), a component not capitalised, a hook not `useX` |
  | `TESSERA_ANALYSIS_RENAME_RESERVED` | a reserved word |
  | `TESSERA_ANALYSIS_RENAME_NO_CHANGE` | the new name is the current one |
  | `TESSERA_ANALYSIS_RENAME_COLLISION` | the name exists: in a module-scope rename, any symbol of a touched file (and any component of the project); in a nested one, a sibling in the scope or at module scope; for a class, a selector of the style sheet |

  Applying the edits and re-analysing adds no diagnostics (tested). Not checked: a second attribute with the new name already on
  a tag, a rename started from the `.css` file, renaming through an `as` alias; renaming an exported component changes the
  name of the generated `gen::<Component>Base`, so the developer class that derives from it needs the same edit.
- **Tests and gate.** `tests/unit/analysis_completion_tests.cpp` (a list per context, ordering, UTF-16, ABI parity),
  `tests/unit/analysis_rename_tests.cpp` (component, prop, state and CSS class across files, every refusal code, re-analysis,
  ABI parity), `scripts/ci/preview_analysis_service.mjs` (request validation, latest-wins completion, the same requests against
  the Wasm module, and the completion budget: with `TESSERA_ANALYSIS_WASM` set, the p95 of 30 completions over
  `samples/gallery` in the worker stays under 100 ms). The editor wiring (completion source, F2 `edit.renameSymbol`, multi-file
  undo) and its Playwright cases are separate studio work.
