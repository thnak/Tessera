# Studio services v1

Status: proposed (V1-M4); the protocol core and its no-UI harness are implemented
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
and restart. It does not define the language requests themselves (definition, hover, completions,
rename); each of those issues adds its request and response types to section 8.

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
| `analysis` | `symbols` (section 10, #570); the rest is defined by the analysis issues (#568 and following) |
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
(`analysis_capabilities_json()`). The worker host that carries it (#568) is not part of the library.

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
`tests/fixtures/corpus/`). The Wasm build and the preview size budget belong to #568's module.
