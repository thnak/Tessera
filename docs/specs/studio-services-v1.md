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
- `delta`: `{ baseVersion, edits: [TextEdit] }`. It is applied only when `baseVersion` equals the
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
| `analysis` | Defined by the analysis issues (#568 and following) |
| `format` | Defined with ADR-061 |
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
swapping the stub's source.
