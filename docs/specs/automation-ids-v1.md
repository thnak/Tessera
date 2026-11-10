# Automation IDs v1: numeric UI/component identity

Status: in progress (Big Steps B2 and C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision: [ADR-027](../../decisions/ADR-027-agent-automation-and-audits.md) (numeric IDs, headless harness, Lighthouse-style audits). The identity and snapshot slice is implemented as `tessera_automation` (`include/tessera/ui/automation/`, `src/ui/automation/`); manifest emission and IR/codegen wiring remain pending. Companion to
[`scene-graph.md`](../design/scene-graph.md), [`ir-v1.md`](ir-v1.md), and
[`headless-automation-v1.md`](headless-automation-v1.md).

Coding agents and traces need stable, numeric identity. Pointers, coordinates, and runtime
instance ids are unstable; string paths are useful for humans but expensive/fragile for tools.

## 1. ID types

| ID | Type | Lifetime | Purpose |
|---|---|---|---|
| `UiId` | `uint64_t` | build-time stable | automation/trace identity for a UI node/component |
| `ComponentId` | `uint64_t` | build-time stable | component identity |
| `NodeId` | `uint64_t` | runtime instance | live scene node identity |
| `AutomationId` | `uint64_t` | build-time, explicit | developer-pinned stable target |
| `ItemKey` | string/uint64 | data lifetime | list/grid instance identity |
| `TraceId`/`SpanId` | `uint64_t` | trace | correlation |

Rules:

- C++ uses `uint64_t`. JSON uses **decimal strings** (JavaScript numbers lose precision above
  2^53).
- `UiId`/`ComponentId` are stable across runs of the same build; `NodeId` is not.
- A list/grid item is identified by `(UiId, ItemKey)`, not by a per-instance `UiId`.
- CSS `id`/class strings remain for styling; they are not automation identity.
- Debug string ids (`node::1`, `comp::Counter`) remain in IR/snapshots for humans.

## 2. Assignment

The compiler assigns numeric IDs:

```text
ComponentId = hash64(project_salt + "|c|" + component_source_path + "|" + component_name)
UiId       = hash64(project_salt + "|u|" + component_source_path + "|" + node_path + "|" + key)
```

- `project_salt` is the `namespace` from `tessera.json` and is required (section 2.1); changing it changes IDs.
- `node_path` is the stable structural path (source order + explicit keys), not a line number.
- `key` is the list key/template key where applicable.
- Explicit `automationId="123456789"` replaces the computed `UiId`: the node's `uiId` then equals
  the `automationId` (section 3); it must be a unique uint64.
- A 64-bit collision is a **hard error**; resolve with an explicit `automationId` or rename.
  Do not rehash/renumber silently.
- Moving/renaming a component/node changes its ID; use `automationId` for long-lived targets.

### 2.1 Hash and salt (implemented)

`hash64` is the first eight bytes of **SHA-256** (FIPS 180-4, implemented in-tree with NIST
known-answer tests) over the salted UTF-8 key, read as a big-endian `uint64`. The salt is the
`namespace` from `tessera.json` and is **required**: an empty salt is a diagnosed error, because
IDs would then differ per checkout.

`include/tessera/ui/automation/id.hpp` owns the contract:

- `make_ui_id_key`/`make_component_id_key` build exactly the strings in section 2 (the trailing
  `|` keeps an empty list key distinct from a shorter path);
- `assign_ui_ids`/`assign_component_ids` assign ids in source order and return
  `std::expected<..., IdError>`; an explicit `automationId` replaces the computed id and is
  flagged as an override;
- a duplicate explicit `automationId`, a zero explicit id, and any 64-bit collision are hard
  errors (`IdErrorCode::DuplicateAutomationId`, `ZeroAutomationId`, `IdCollision`). Nothing is
  silently last-wins or renumbered.

Manifest (`generated/automation.manifest.json`):

```json
{
  "manifestVersion": 1,
  "projectSalt": "factory",
  "components": [
    { "componentId": "1234567890123456789", "name": "Counter", "source": "ui/Counter.tsx" }
  ],
  "nodes": [
    {
      "uiId": "987654321098765432",
      "componentId": "1234567890123456789",
      "kind": "Button",
      "path": "Counter/root[0]/Button[1]",
      "source": { "file": "ui/Counter.tsx", "line": 12, "column": 5 }
    }
  ]
}
```

- Manifest is deterministic and reviewed like generated code.
- `uiId`/`componentId` map to debug path/name/source for agents and traces.
- The manifest is the source of truth for automation; no runtime discovery of string paths.

## 3. IR fields

The IR (the compiler's intermediate representation, [`ir-v1.md`](ir-v1.md)) keeps human-readable `id` strings and adds numeric identity:

```json
{
  "id": "comp::Counter",
  "componentId": "1234567890123456789",
  "nodes": [
    {
      "id": "node::button",
      "uiId": "987654321098765432",
      "automationId": null,
      "kind": "Button"
    }
  ]
}
```

- `componentId`/`uiId` are required in generated IR and unique per project.
- `automationId` is optional; if set, `uiId` equals it (an explicit `automationId` replaces the computed `UiId`, section 2).
- Validation rejects duplicate/missing numeric IDs and collisions.
- Codegen emits constructors with `(NodeId, UiId, ComponentId)`.

## 4. Scene graph

`Node` (the scene graph node, [`scene-graph.md`](../design/scene-graph.md)) carries:

```cpp
NodeId node_id_;          // runtime, monotonic
UiId ui_id_;              // build-time stable
ComponentId component_id_;
```

- `NodeId` is unique among live nodes; no reuse within a process.
- `UiId` may repeat for list/grid item instances; use `ItemKey` to disambiguate.
- Snapshot/query never exposes raw pointers.
- Hidden/offscreen nodes may be omitted from snapshots unless requested.

## 5. Snapshot and query API

```cpp
struct UiSnapshotNode {
    NodeId node_id;
    UiId ui_id;
    ComponentId component_id;
    NodeId parent;
    Role role;
    std::string name;          // accessible name
    std::string text;          // text content, optional
    std::string value;         // input value, optional
    Rect bounds;               // logical pixels
    bool visible;
    bool enabled;
    bool focused;
    bool selected;
    std::string item_key;      // list/grid instance, optional
    std::string element_id;    // CustomElement id, optional
    std::vector<NodeId> children;
};

struct UiSnapshot {
    std::uint64_t sequence;
    std::vector<UiSnapshotNode> nodes;   // stable order
};
```

Query/actions:

```cpp
Result<UiSnapshot> snapshot(SnapshotOptions = {});
Result<std::vector<NodeId>> find_by_ui_id(UiId, std::optional<ItemKey> = {});
Result<std::vector<NodeId>> find_by_component(ComponentId);
Result<std::vector<NodeId>> find_by_role(Role, std::string_view name = {});
Result<NodeId> wait_for(UiId, Predicate, Duration timeout);
Result<void> click(UiId, std::optional<ItemKey> = {});
Result<void> type_text(UiId, std::string_view);
Result<void> scroll(UiId, float dx, float dy);
Result<void> focus(UiId);
```

- Actions go through the normal input/runtime path; they do not mutate state directly.
- `wait_for` polls/observes at frame boundaries with a bounded timeout.
- Snapshot order is deterministic (tree preorder or sorted by NodeId; not decided yet, see section 9).
- Bounds are logical pixels; DPI/scale is explicit in the snapshot header.

### 5.1 Custom element sub-targets

- A custom element can expose `(UiId, partId)` for 3D picking/sub-targets; `partId` is a
  `uint32` assigned by the element and stable for a build.
- `partId` is meaningful only within its element; never use it as a global id.
- Snapshot includes `element_id`, current `part_id`, and the element's state/draw stats.
- Input scripts may click/assert with `uiId` + optional `partId`.
- Trace spans may carry `partId`; use `uiId`/`element_id` for correlation.
- See [`custom-elements-v1.md`](custom-elements-v1.md).

### 5.2 Snapshot slice (implemented)

`include/tessera/ui/automation/snapshot.hpp` and `snapshot_json.hpp` provide a plain-struct
builder and stable serializer for the automation subset of the snapshot; they do not depend on
the scene graph or the compiler's JSON types, so the compiler, codegen fixtures and the runtime
feed the same tree.

- Order is tree **preorder (document order)**: parent before children, children in source order.
  `parent` is the entry index of the ancestor (`null` for a root); `depth` is the nesting level.
- A hidden/offscreen node is omitted **with its subtree** unless the caller sets `include_hidden`,
  which emits it with `visible=false`.
- A list/grid item carries `(uiId, itemKey)`; a repeated `UiId` is resolved by
  `find_by_ui_id(snapshot, ui_id, item_key)`.
- `snapshot_to_json` writes canonical one-line JSON with a fixed field order: `snapshotVersion`,
  `sequence`, then `nodes`. `uiId`, `componentId` and `sequence` are decimal strings (JavaScript
  loses precision above 2^53), `parent` is a decimal index or `null`, `bounds` values are fixed
  three-decimal numbers, strings are JSON-escaped with UTF-8 passed through, and the document
  ends with one newline.

## 6. Tracing

Every event/span may carry:

```json
{
  "traceId": "1234567890",
  "spanId": "987654321",
  "uiId": "987654321098765432",
  "componentId": "1234567890123456789",
  "nodeId": "42",
  "elementId": "Waterfall3D",
  "partId": "7",
  "name": "event.click-handler",
  "startUs": 123456,
  "durationUs": 850
}
```

- IDs are strings in JSON; uint64 in C++.
- Trace spans: input → handler → style → layout → paint → present.
- The automation manifest resolves numeric IDs to source paths/names.
- No PII (personally identifiable information) or secrets in span attributes; redact.

## 7. Stability and versioning

- Same source/config -> same numeric IDs (byte-identical manifest).
- IDs change on move/rename/structural change; document and use explicit `automationId` for
  long-lived agent targets.
- Manifest version bumps on schema changes; old IDs remain valid for the same manifest.
- Do not derive IDs from pointers, addresses, random values, or runtime order.
- Do not renumber all IDs when one node is added; hash-based assignment avoids this.

## 8. Tests

The implemented slice runs as CTest targets `automation_sha256`, `automation_hash_id`,
`automation_snapshot` and `automation_snapshot_json` under `tests/unit/automation/`:
NIST SHA-256 known answers, fixed hash64 known answers, namespace sensitivity, computed-vs-explicit
UiIds, duplicate/colliding id diagnostics, preorder/hidden policy, `(UiId, ItemKey)` lookup, and
byte-identical canonical JSON (including uint64 values above 2^53).

Still required for the full section (not produced by this slice):

- Deterministic manifest (`generated/automation.manifest.json`) across repeated builds.
- IR validation and codegen emission of `componentId`/`uiId`.
- Query/actions through the real input path.
- Trace correlation (IDs present, correct span names).
- Move/rename ID change is documented; `automationId` stays stable.

## 9. Open questions

Resolved (normative here; mirrored in [`docs/reference/open-questions.md`](../reference/open-questions.md)):

- **Hash algorithm and project-salt source** — in-tree SHA-256 with NIST known-answer tests;
  `hash64` is its first eight bytes read big-endian. The salt is the `tessera.json` `namespace`
  and is required (sections 2, 2.1).
- **Snapshot order and hidden-node policy** — tree preorder (document order); hidden/offscreen
  nodes are omitted with their subtree unless `includeHidden` is set (sections 5, 5.2).
- **Computed vs explicit-only `UiId`** — computed source-path/keys `UiId`s are the default; an
  explicit `automationId` overrides a node's `UiId` and must be unique; explicit-only is rejected
  (sections 2 and 3).
- **Window scoping** — one window per snapshot, selected by an optional `windowId` that defaults
  to the focused window; cross-window aggregation is deferred.
- **`partId` stability/packing** — `partId` is a `uint32` assigned by the element, stable for a
  build, emitted as its own JSON field alongside `uiId`/`elementId`; never packed into `UiId`.
- **ID stability across hot reload** — IDs are pure functions of `(salt, source path, node path,
  key)`, so unchanged nodes keep their IDs; changed subtrees re-ID and the manifest diff shows it.
- **Encoding for very large snapshots** — canonical JSON with uint64 decimal strings; very large
  trees stream NDJSON per node; zstd is opt-in only after a >16 MiB measurement.

Not yet implemented by the identity/snapshot slice: manifest emission, `windowId` in the snapshot
header, `partId` fields, NDJSON streaming, and zstd.
