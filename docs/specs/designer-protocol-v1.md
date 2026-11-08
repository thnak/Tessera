# Designer protocol v1

Status: proposed. It is not mapped to a Big Step ([AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)); see the [glossary](../reference/glossary.md) entry "V1".
Decision record: [ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) (self-contained Wasm designer).
Design note: [`designer.md`](../design/designer.md).

This document is the normative definition of the messages exchanged between the **host** (an editor
or the native CLI) and the **viewer** (the Wasm designer engine running in a browser page). It
defines message names, fields, ordering and error behavior. It does not define how the engine
evaluates IR ([`ir-v1.md`](ir-v1.md)), which source is accepted ([`jsx-subset-v1.md`](jsx-subset-v1.md))
or which style values are valid ([`stylesheet-v1.md`](stylesheet-v1.md)).

## 0. How to read this document

### 0.1 Terms

| Term | Meaning in this document |
|---|---|
| Host | The program that owns the project files: an editor integration or the native `tessera` CLI ([`designer.md`](../design/designer.md) section 2). It reads and writes files |
| Viewer | The static Wasm engine plus its JavaScript shell in the browser page ([ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md)). It never reads or writes project files |
| Message | One JSON object sent in one direction (section 2) |
| Request / response | A message with an `id` and the message that answers it with the same `id` |
| Event | A message that needs no answer |
| Project file | A file listed by `project.load`: a `.tsx`, `.jsx`, `.css` or token file |
| File version | A non-negative integer chosen by the host for each project file; it increases every time the host's copy of that file changes |
| NodeRef | The identity of one scene node: `componentId`, `instanceKey`, `nodeId` (section 3.2) |
| Pick mode | Viewer state in which pointer hover highlights nodes and a click selects one ([`designer.md`](../design/designer.md) section 8.1) |
| Override | A style value set in the viewer's designer override layer, not yet written to a source file ([`designer.md`](../design/designer.md) section 8.2) |
| Commit | Turning overrides into text edits the host applies (section 8) |
| TextEdit | One replacement of a byte range of a file by new text (section 3.3) |
| V1 | The first release; see the [glossary](../reference/glossary.md) |

### 0.2 References

| Reference | Subject |
|---|---|
| [`designer.md`](../design/designer.md) | Architecture, picker (8.1), style editing (8.2), hot reload (4) |
| [`ir-v1.md`](ir-v1.md) | `componentId`, `uiId`, node `id`, `source` span |
| [`automation-ids-v1.md`](automation-ids-v1.md) | Definition and encoding of `componentId` and `uiId` |
| [`jsx-subset-v1.md`](jsx-subset-v1.md) | Diagnostics format (13.1), spans (13.2), error-tolerant parsing (14) |
| [`stylesheet-v1.md`](stylesheet-v1.md) | Cascade origins (7.1), diagnostics (15), compatibility matrix (11) |
| [`devtools-protocol-v1.md`](devtools-protocol-v1.md) | The separate protocol for inspecting a running native app; not used by the designer |
| [`virtualization-v1.md`](virtualization-v1.md) | Items that exist only while scrolled into the scene |

## 1. Scope and non-goals

In scope: loading a project, sending changed source, reporting diagnostics, picking and selecting
nodes, inspecting and overriding style, and returning text edits for the host to apply.

Not in scope:

- Editing component props or children (no release scheduled, [`designer.md`](../design/designer.md) section 8.2).
- Writing files from the viewer. Only the host writes files.
- Editing developer C++ or any text inside a protected region ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)).
- The transport (WebSocket, `postMessage`, stdio). This document defines the message content only;
  the transport is not specified yet (section 14).
- Binary encoding. All messages are JSON ([`designer.md`](../design/designer.md) section 3).

## 2. Envelope

Every message is a JSON object:

```json
{ "v": 1, "type": "source.update", "id": "17", "payload": { } }
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `v` | integer | yes | Protocol major version. This document defines `1` |
| `type` | string | yes | Message name from sections 5 and 6 |
| `id` | string | for requests and responses | Chosen by the sender of the request: a decimal string, unique among that sender's open requests. The response repeats it |
| `payload` | object | yes | Fields defined per message; `{}` if none |

Rules:

- Identifiers of the form `componentId`, `nodeId` and sizes that may exceed 2^53 are decimal
  strings, as in [`automation-ids-v1.md`](automation-ids-v1.md). Other numbers are JSON numbers.
- A receiver ignores unknown fields inside `payload`.
- A receiver answers an unknown `type` with an `error` event (code `TESSERA_DESIGNER_UNKNOWN_MESSAGE`)
  and otherwise ignores it; the session continues.
- A receiver answers a message that is not valid JSON, lacks `v`/`type`, or has a `payload` of the
  wrong shape with an `error` event (code `TESSERA_DESIGNER_MALFORMED`); the session continues.
- A message with a different major `v` is answered with `error` code `TESSERA_DESIGNER_VERSION` and
  is not processed. Minor changes in this protocol add optional fields and new message types only.
- Messages from one sender are processed in the order sent. There is no ordering guarantee between
  the two directions.
- Maximum message size and the maximum number of open requests are not specified yet (section 14).

## 3. Common types

### 3.1 SourceSpan

```json
{ "file": "ui/Counter.tsx", "offset": 120, "length": 14, "start": [8, 5], "end": [8, 19] }
```

`file` is a project-relative path with POSIX separators. `offset` and `length` are in bytes of the
UTF-8 file. `start` and `end` are `[line, column]`, both 1-based; a column counts Unicode code points
from the start of the line ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 13.2). `end` is exclusive.

### 3.2 NodeRef

```json
{ "componentId": "1234567890123456789", "instanceKey": "item-7", "nodeId": "node::2" }
```

| Field | Meaning |
|---|---|
| `componentId` | uint64 decimal string from the IR component ([`ir-v1.md`](ir-v1.md) section 2) |
| `instanceKey` | Identifies one instance among all instances of the same component. It is the `key` values of the enclosing list items, outermost first, joined with `/`; it is the empty string when the component instance is not inside any list. Defined in [`ir-v1.md`](ir-v1.md) section 3.1.1 |
| `nodeId` | The node's debug `id` string within the component ([`ir-v1.md`](ir-v1.md) section 3) |

Two NodeRefs are equal when all three fields are equal.

### 3.3 TextEdit

```json
{ "file": "ui/Counter.tsx", "offset": 120, "length": 14, "newText": "style={{ gap: 8 }}" }
```

Replace `length` bytes starting at byte `offset` of `file` by `newText` (UTF-8). `length` 0 inserts.
`offset` and `offset + length` are on UTF-8 code point boundaries.

### 3.4 Diagnostic

The diagnostic object defined in [`jsx-subset-v1.md`](jsx-subset-v1.md) section 13.1 (`code`,
`severity`, `message`, `span`, optional `suggestion`, optional related spans) with `span` encoded as
SourceSpan (3.1). Style diagnostics use the codes of [`stylesheet-v1.md`](stylesheet-v1.md) section 15.

## 4. Session lifecycle

```text
host                                   viewer
 | -- hello ----------------------------> |   (protocol versions the host supports)
 | <------------------------- ready ----- |   (chosen version, engine build id, capabilities)
 | -- project.load ----------------------> |
 | <---------------------- project.state - |   (per-component status after first parse)
 | -- source.update / ... ---------------> |
```

- `hello` payload: `{ "versions": [1] }`. `ready` payload: `{ "version": 1, "engine": "<build id>",
  "capabilities": ["pick", "style.edit"] }`. If no version matches, the viewer sends `error` with
  `TESSERA_DESIGNER_VERSION` and sends no `ready`.
- The viewer ignores every message except `hello` until it has answered it.
- The host sends `project.load` once per session. Loading another project requires a new session.
- The session ends when the transport closes. The viewer keeps showing its last frame and does not
  crash; the host may open a new session, which starts with `hello` again.

## 5. Messages: host to viewer

| `type` | Kind | Payload fields | Behavior |
|---|---|---|---|
| `hello` | request | `versions` | Section 4 |
| `project.load` | request | `config` (project configuration object), `files` (array of `{path, version, text}`), `assets` (array of `{path, version}`; bytes are fetched by the shell, not sent in JSON) | Replaces all viewer state; answered by `project.state` |
| `source.update` | event | `files`: array of `{path, version, text}` | Section 7 |
| `asset.update` | event | `assets`: array of `{path, version}` | Font, image or theme asset changed; viewer reloads those assets and requests one frame |
| `viewport.set` | event | optional `width`, `height`, `dpr`, `theme` (`"light"`, `"dark"` or a theme id from the project), `reducedMotion` (boolean), `safeAreas`, `foldPosture` | Sets the preview environment ([`designer.md`](../design/designer.md) section 8); omitted fields keep their value. `theme` is the value of `useTheme()` and `width` determines `useBreakpoint()` ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1) |
| `pick.mode` | event | `enabled` (boolean) | Section 8.1 |
| `selection.set` | request | one of: `node` (NodeRef), `source` (`{file, offset}`), `parent` (true), `none` (true) | Section 8.2; answered by `selection.set.result` |
| `style.inspect` | request | `node` (NodeRef) | Answered by `style.info` (section 9.1) |
| `style.override.set` | request | `node` (NodeRef), `property` (CSS property name), `value` (string) | Section 9.2; answered by `style.override.result` |
| `style.override.clear` | request | `node` (NodeRef) and optional `property`; without `property` clears all overrides of that node | Answered by `style.override.result` |
| `style.override.revert` | event | none | Clears every override |
| `style.commit` | request | `node`, `destination`, destination fields (section 10) | Answered by `style.commit.result` and followed by a `source.patch` event |
| `source.patch.result` | event | `patchId`, `status` (`"applied"` or `"rejected"`), optional `reason` | Host's answer to a `source.patch` (section 10.3) |
| `edit.undo` / `edit.redo` | event | none | Undo or redo the last override operation (section 9.4) |

## 6. Messages: viewer to host

| `type` | Kind | Payload fields | When sent |
|---|---|---|---|
| `ready` | response to `hello` | Section 4 | Once |
| `project.state` | response to `project.load` | `components` (array of ComponentState, section 7.3) | After the first parse of every file |
| `source.applied` | event | `files`: array of `{path, version}`, `components` (array of ComponentState) | After each `source.update` is processed (section 7.2) |
| `diagnostic` | event | `file`, `version`, `diagnostics` (array of Diagnostic) | The full diagnostic list of one file, replacing the previous list for that file at that version or older. An empty array clears the file's diagnostics |
| `error` | event | `code`, `message`, optional `ref` (`id` of the offending message) | A protocol-level or engine-level failure that is not a source diagnostic (section 12) |
| `hover.changed` | event | `node` (NodeRef or `null`), `span` (SourceSpan or `null`) | Pick mode is on and the node under the pointer changed |
| `selection.changed` | event | `node` (NodeRef or `null`), `span` (SourceSpan or `null`), `path` (array of NodeRef from the root to the node), `cause` (`"click"`, `"host"`, `"reload"`) | The selection changed (section 8.2) |
| `selection.set.result` | response | `ok` (boolean), optional `reason` | Answer to `selection.set` |
| `style.info` | response | Section 9.1 | Answer to `style.inspect` |
| `style.override.result` | response | `ok`, `diagnostics`, `overrides` (array of `{node, property, value}` after the operation) | Answer to an override request |
| `style.commit.result` | response | `ok`, optional `reason`, optional `patchId` | Section 10 |
| `source.patch` | event | `patchId`, `baseVersions` (object path → file version), `edits` (array of TextEdit) | Section 10.2 |
| `state.patch` | event | `components` (array of `{componentId, instanceKey, state}`) | Optional state snapshot taken before a reload so the viewer can restore it afterwards; its format is not specified yet (section 14) |

## 7. Source synchronization

### 7.1 Versions

The host assigns each project file a version. `project.load` and `source.update` carry the full text
of each listed file (not a diff) and its version.

- The viewer ignores a file entry whose version is less than or equal to the version it has already
  applied for that file, and reports nothing for it.
- The host never sends two different texts with the same version for one file.

### 7.2 Processing

For each changed file the viewer runs the front end in recovery mode
([`jsx-subset-v1.md`](jsx-subset-v1.md) section 14), updates the IR of every `Valid` component, keeps
the last valid IR of every `Invalid` component, evaluates the IR diff, and requests one frame
([`designer.md`](../design/designer.md) sections 2.1 and 4). It then sends, in this order: `diagnostic`
for each changed file, then `source.applied`. If several `source.update` messages are queued, the
viewer may process them as one batch; it still sends one `source.applied` per processed batch that
lists the highest version applied per file.

A CSS file that fails to parse produces `diagnostic` events with `TESSERA_CSS_*` codes; the rules
that parsed are applied and the invalid ones are skipped, as [`stylesheet-v1.md`](stylesheet-v1.md)
section 15 specifies.

### 7.3 ComponentState

```json
{ "componentId": "1234567890123456789", "name": "Counter", "file": "ui/Counter.tsx",
  "status": "valid", "span": { "file": "ui/Counter.tsx", "offset": 0, "length": 310,
  "start": [1, 1], "end": [14, 2] } }
```

`status` is `"valid"`, `"invalid"` (the viewer shows the last valid IR for this component), or
`"never_valid"` (the component has no valid IR yet and is not rendered; its place shows an error
placeholder). `span` is the component declaration's span in the version that was processed.

## 8. Picker

### 8.1 Pick mode

- `pick.mode { "enabled": true }` starts pick mode. While it is on, the viewer sends
  `hover.changed` when the node under the pointer changes, at most once per rendered frame, and a
  click selects the node under the pointer instead of being delivered to the UI. The click is
  consumed: it does not reach any component handler.
- While pick mode is off, the viewer routes input to the UI normally and sends no `hover.changed`.
- The node under the pointer is the deepest component instance's root-most node by default; finer
  selection uses `selection.set` with `node` or `parent` ([`designer.md`](../design/designer.md)
  section 8.1).

### 8.2 Selection

- `selection.set` with `node` selects that NodeRef; with `source` selects the deepest node whose
  source span contains the byte `offset` of `file` (this is how an editor cursor selects a node);
  with `parent` selects the parent of the current selection; with `none` clears the selection.
- If the target does not exist (stale NodeRef, offset outside any node, no current selection), the
  answer is `ok: false` with `reason` one of `"no_such_node"`, `"no_node_at_offset"`,
  `"no_selection"`, and the selection is unchanged.
- Every selection change sends `selection.changed`. `cause` is `"click"` for a pick click, `"host"`
  for `selection.set`, `"reload"` when a source update changed the selected node.
- After a source update, the viewer restores the selection by matching `(componentId, instanceKey)`
  and `nodeId`; if no node matches, it selects the nearest ancestor that does and sends
  `selection.changed` with `cause: "reload"`; if none matches, the selection becomes `null`
  ([`designer.md`](../design/designer.md) section 8.1).
- An item of a virtualized list can be selected only while it exists in the scene
  ([`virtualization-v1.md`](virtualization-v1.md)); `selection.set` for an item that does not exist
  answers `no_such_node`.

## 9. Style inspection and overrides

### 9.1 `style.info`

Response payload:

| Field | Meaning |
|---|---|
| `node` | The NodeRef inspected |
| `computed` | Object: property name → resolved value string, for every property with a non-initial value |
| `matched` | Array, highest precedence first: `{origin, selector, specificity, span, declarations}` where `origin` is `"inline"`, `"author"`, `"theme"` or `"ua"` ([`stylesheet-v1.md`](stylesheet-v1.md) section 7.1; `"theme"` marks a declaration from a token file), `specificity` is `[ids, classes, types]`, `span` is the SourceSpan of the rule (the inline `style` for `"inline"`) and `declarations` lists `{property, value, important, overridden}` |
| `box` | Layout box in logical pixels: `{x, y, width, height, margin, border, padding}` each a 4-number array top, right, bottom, left for the last three |
| `overrides` | The node's current overrides |

If the NodeRef does not exist, the response is `ok: false` with `reason: "no_such_node"` and no
other fields.

### 9.2 Setting an override

`style.override.set` applies the value in the override layer above the author and inline origins
(it wins over them for the same property). The viewer validates `property` and `value` with the
stylesheet engine, the same code the compiler uses
([`stylesheet-v1.md`](stylesheet-v1.md) section 15):

- Valid: `ok: true`; the viewer restyles only (no re-parse), requests one frame, and the override
  is marked uncommitted in the viewer UI.
- Invalid: `ok: false`; `diagnostics` holds the `TESSERA_CSS_*` diagnostic; the node's overrides
  are unchanged.

Setting an override changes only that NodeRef's node, not other instances of the same component.
Overrides are lost when the session ends and are never written without a commit.

### 9.3 Clearing

`style.override.clear` removes one override or all overrides of a node. `style.override.revert`
removes every override in the viewer.

### 9.4 Undo and redo

The viewer keeps an undo stack of override operations (`set`, `clear`, `revert`) for the session.
`edit.undo` and `edit.redo` act on it. A commit is not undoable by `edit.undo`: its effect is in
the host's files, and the host's own undo applies. After a commit the stack is cleared of the
operations the commit consumed.

## 10. Commit

### 10.1 Request

`style.commit` payload: `node` (NodeRef) plus `destination`:

| `destination` | Extra fields | What is edited |
|---|---|---|
| `"inline"` | none | The `style` attribute of the JSX element for `node` in its component source; it is created if absent. The change affects every instance of that component, because it edits the source |
| `"class"` | `className` (string) | The rule for `.className` in the project CSS file named by `file` (extra field); the rule is appended to that file if missing. If the element does not list `className`, the `className` attribute is edited to add it |
| `"token"` | `token` (string, for example `--accent`) | The value of the existing token declaration in the token file; the node's override must be of a property whose value is `var(<token>)` or the commit is rejected with `reason: "not_token_bound"` |

All of the node's current overrides are committed together. A commit of a node with no overrides
answers `ok: false`, `reason: "no_overrides"`.

### 10.2 `source.patch`

If the commit can be turned into edits, the viewer answers `style.commit.result` with `ok: true` and a
`patchId`, then sends `source.patch` with that `patchId`:

- `baseVersions` maps each affected file path to the file version the viewer used to compute offsets.
- `edits` is the minimal list of TextEdits, non-overlapping, sorted by file and descending offset so
  applying them in order never shifts a later edit. Text outside the replaced ranges (formatting,
  comments, the `// Note: bind::<id>` comments and their handlers) is never part of an edit.
- The patch is atomic: the host applies all edits or none.

If the commit cannot be turned into edits, `style.commit.result` is `ok: false` with `reason` one of
`"no_overrides"`, `"not_token_bound"`, `"no_such_node"`, `"no_such_class_file"`, `"component_invalid"`
(the component has no valid source span to edit), `"stale_source"` (the viewer's version of a file
is older than a version it was told about). No `source.patch` is sent.

### 10.3 Host's answer

The host compares each `baseVersions` entry with the file's current version.

- All equal: the host applies the edits and sends `source.patch.result` with `status: "applied"`,
  followed by a `source.update` with the new texts and incremented versions.
- Any differs: the host applies nothing and sends `source.patch.result` with `status: "rejected"`,
  `reason: "version_mismatch"`. The viewer keeps the overrides so the developer can commit again
  after the newer source arrives. The host may also reject for other reasons (file not writable)
  and put the reason text in `reason`.

Until `source.patch.result` arrives the viewer keeps the overrides. On `applied` and after it has
processed the following `source.update`, it removes the overrides that were committed and checks
that the new IR gives the node the computed values of the committed overrides; if it does not, it
keeps the override and sends a `diagnostic` with `TESSERA_DESIGNER_PATCH_MISMATCH`.

The viewer sends no new `source.patch` for the same node while a patch for it is open.

## 11. Ordering and flow control

- The viewer processes host messages one at a time, in order. A `source.update` never interrupts a
  frame being rendered; its result appears in a following frame.
- The viewer may merge consecutive `style.override.set` requests for the same node and property
  while they are queued, answering every merged request with the result of the last.
- `hover.changed` and `selection.changed` are coalesced to the latest state per rendered frame.
- The viewer never blocks the browser main thread on a host message
  ([AGENTS.md non-negotiable 12](../../AGENTS.md#non-negotiables)); processing that could take
  long is run in a way that keeps the page responsive. How is not specified (section 14).

## 12. Errors

`error` payload: `code`, `message`, optional `ref`. Codes:

| Code | Cause |
|---|---|
| `TESSERA_DESIGNER_MALFORMED` | Invalid JSON, missing field, wrong type |
| `TESSERA_DESIGNER_UNKNOWN_MESSAGE` | Unknown `type` |
| `TESSERA_DESIGNER_VERSION` | No common protocol version, or `v` not 1 |
| `TESSERA_DESIGNER_PATH` | A file path is absolute, contains `..`, or is outside the project |
| `TESSERA_DESIGNER_LIMIT` | A message or file exceeds a size limit |
| `TESSERA_DESIGNER_NOT_READY` | A message other than `hello` arrived before `ready` was sent, or a message other than `hello` and `project.load` arrived before `project.load` was answered |
| `TESSERA_DESIGNER_PATCH_MISMATCH` | A committed edit did not produce the expected style (section 10.3); sent as a `diagnostic` |
| `TESSERA_DESIGNER_ENGINE` | An internal engine failure (for example a WebGPU validation error) turned into a report instead of a crash |

An error never ends the session and never leaves the viewer without a frame.

## 13. Security

- The protocol is local-only: the host binds only to the loopback interface or to a transport with
  the same property, and the viewer accepts messages only from the origin that loaded it
  ([`designer.md`](../design/designer.md) section 7).
- A path in any message is project-relative with POSIX separators; the receiver rejects an absolute
  path, a `..` segment or a path that resolves outside the project with `TESSERA_DESIGNER_PATH`.
- The host applies an edit only to files of the project and only to UI sources (`.tsx`, `.jsx`,
  `.css`, token files). It rejects an edit to any other file, including C++ and manifests.
- File text and messages are untrusted input: limits are enforced before parsing and a failure is
  an error message, never a crash.
- No message carries executable code. The viewer executes IR only, never text from a message.

## 14. Open questions (no decision yet)

- Transport between host and shell (WebSocket, `postMessage` from an editor webview, stdio) and the
  maximum message size, the maximum open requests, and the size limits for files and assets.
- Content and encoding of `state.patch` and whether state restoration is part of V1.
- Whether the viewer may run the front end in a Web Worker ([`designer.md`](../design/designer.md)
  section 10) and, if so, how section 11 changes.
- Keyboard modifiers for picking an ancestor or primitive by click
  ([`designer.md`](../design/designer.md) section 8.1).
- Whether `style.commit` to `"class"` may create a new CSS file.
- Whether the designer can run with no host ([`designer.md`](../design/designer.md) section 10); this
  protocol assumes a host exists.

## 15. Tests

- Handshake: version match and mismatch, messages before `ready`, repeated `hello`.
- Envelope: malformed JSON, unknown `type`, unknown fields ignored, wrong `v`.
- Source sync: stale versions ignored, batch merging, `Invalid` and `never_valid` components,
  diagnostics replaced per file, CSS partial application.
- Picker: hover coalescing, click consumed, selection by source offset, restore after reload with
  match, with ancestor fallback, and with none.
- Style: override validation errors, precedence over inline and author, clear/revert/undo/redo, merge
  of queued sets.
- Commit: each destination, atomic multi-file patch, edit ordering, comment and `bind::` preservation,
  `version_mismatch`, `PATCH_MISMATCH` detection, patch round trip gives the same IR and clears the
  override ([`designer.md`](../design/designer.md) section 9).
- Security: path rejection, edit to a non-UI file rejected, oversized input.
