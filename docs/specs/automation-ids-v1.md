# Automation IDs v1: numeric UI/component identity

Status: in progress (Big Steps B2 and C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision: [ADR-027](../../decisions/ADR-027-agent-automation-and-audits.md) (numeric IDs, headless harness, Lighthouse-style audits). The identity and snapshot slice is implemented as `tessera_automation` (`include/tessera/ui/automation/`, `src/ui/automation/`); the compiler assigns the ids (`tessera_ir_ids`; the codegen CLI reads `app.namespace` from `tessera.json`, or takes `--salt`/`--root`) and codegen sets them on the scene nodes and writes the manifest; the harness does not read the manifest yet. Companion to
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
ComponentId = hash64(project_salt + "|c|" + component_source_path + "|" + component_name
                     + ["|#" + occurrence])
UiId       = hash64(project_salt + "|u|" + component_source_path + "|" + node_path + "|" + key)
```

- `project_salt` is the `namespace` from `tessera.json` and is required (section 2.1); changing it changes IDs.
- `node_path` is the stable structural path (source order + explicit keys), not a line number. Its
  first segment is the component name; when a source file declares the same name more than once, the
  second and later occurrences are written `<Name>#1`, `<Name>#2`, ... (section 2.1).
- `occurrence` is the zero-based position among components with the same name in the same source
  file. It is omitted for the first occurrence, so every ordinary (uniquely named) component keeps
  exactly the id it had before same-named duplicates were disambiguated.
- `key` is the list key/template key where applicable.
- Explicit `automationId="123456789"` replaces the computed `UiId`: the node's `uiId` then equals
  the `automationId` (section 3); it must be a unique uint64.
- A 64-bit collision is a **hard error**; resolve with an explicit `automationId` or rename.
  Do not rehash/renumber silently.
- Moving/renaming a component/node changes its ID; use `automationId` for long-lived targets.
- Components have no explicit id prop in the language: `id` is an element prop on a node (CSS
  identity), not a component identity override. `automationId` is the explicit, per-node pin; a
  component's identity is derived only. Component ids therefore change only on rename,
  source-path change, or same-name occurrence changes (section 7).

### 2.1 Hash and salt (implemented)

`hash64` is the first eight bytes of **SHA-256** (FIPS 180-4, implemented in-tree with NIST
known-answer tests) over the salted UTF-8 key, read as a big-endian `uint64`. The salt is the
`namespace` from `tessera.json` and is **required**: an empty salt is a diagnosed error, because
IDs would then differ per checkout.

`include/tessera/ui/automation/id.hpp` owns the contract:

- `make_ui_id_key`/`make_component_id_key` build exactly the strings in section 2 (the trailing
  `|` keeps an empty list key distinct from a shorter path); `make_component_id_key` takes the
  occurrence among same-named components of the file and appends `|#N` only for `N > 0`;
- `assign_ui_ids`/`assign_component_ids` assign ids in source order and return
  `std::expected<..., IdError>`; an explicit `automationId` replaces the computed id and is
  flagged as an override;
- `assign_component_ids` derives each component's occurrence from the input order (same source
  path and name), never from the absolute index in the module, so two same-named components get
  distinct ids and inserting or reordering a differently named component moves nothing;
- a duplicate explicit `automationId`, a zero explicit id, and any 64-bit collision are hard
  errors (`IdErrorCode::DuplicateAutomationId`, `ZeroAutomationId`, `IdCollision`). Nothing is
  silently last-wins or renumbered.
- The first-party library components (`Field`, `Form`, ...) are stamped at build time by
  `assign_library_identity` with the fixed salt `tessera.library` (`kLibraryIdSalt` in
  `src/compiler/ids/`), because they are compiled into the framework once rather than generated
  per project. Their ids are project-independent, and inserting another library component cannot
  move them; the generator emits no index-derived identity band. This replaces the earlier
  `900000000000000001 + N` band, where inserting a component alphabetically shifted the ids (and
  the committed goldens) of every later component; those band ids are not preserved (the
  framework has no released id contract yet) and the regenerated `Field`/`Form` ids are the new
  ones.

Manifest (`generated/automation.manifest.json`; the codegen CLI writes it as `automation.manifest.json` in its `--out` directory whenever ids are assigned, and `--check` covers it; `write_manifest` in `src/compiler/ids/`, keys in byte order, nodes in document order, `path` as hashed):

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
      "name": "Increment",
      "path": "Counter/root[0]/Button[1]",
      "source": { "file": "ui/Counter.tsx", "line": 12, "column": 5 }
    }
  ]
}
```

- `name` is optional: a static string `aria-label`, else a static `label` prop (button text), else the
  node's own literal text. A dynamic `aria-label` hides the fallbacks, and a node with no static
  source omits it.
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

The precise contract (implemented in `src/compiler/ids/` and `tessera_automation`, tested by
`ir_ids`, `ir_ids_stability` and `automation_hash_id`):

- Component identity is a pure function of `(salt, source path, name, occurrence among same-named
  components of that file)`; node identity is a pure function of `(salt, source path, node path,
  key)`. Neither depends on the component's absolute index in the module, on the number of
  components, or on build/runtime order.
- **Inserting a differently named component**, before or after an existing one, changes no
  existing component's id, no existing node's id, and (for codegen) no byte of the generated files
  of the components that did not change.
- **Inserting a new component after an existing one** likewise changes nothing that already
  existed. **Inserting another component with the same name in the same source file** renumbers
  only the occurrences at and after the insertion point; components with other names are untouched.
- **Reordering two same-named components is an intentional identity change**: identity is
  positional among the same-named declarations, so the two declarations exchange ids. `node_path`
  carries the same occurrence suffix (`<Name>#1`, ...), so their nodes exchange ids as well.
- **Renaming or moving a component/node** changes its id, and so does editing a node's structural
  position or literal key. Use an explicit `automationId` for long-lived agent targets.
- Same source/config produces the same ids and a byte-identical manifest. Manifest version bumps
  on schema changes; old ids remain valid for the same manifest.
- Do not derive ids from pointers, addresses, random values, or runtime order.
- Do not renumber all ids when one component or node is added; hash-based assignment avoids this.
- Framework library components are stamped with the fixed `tessera.library` salt, so their ids are
  project-independent and do not move when another library component is added (section 2.1).

## 8. Tests

The implemented slice runs as CTest targets `automation_sha256`, `automation_hash_id`,
`automation_snapshot` and `automation_snapshot_json` under `tests/unit/automation/`:
NIST SHA-256 known answers, fixed hash64 known answers, namespace sensitivity, computed-vs-explicit
UiIds, duplicate/colliding id diagnostics, preorder/hidden policy, `(UiId, ItemKey)` lookup, and
byte-identical canonical JSON (including uint64 values above 2^53).

`ir_ids` (`tests/unit/ir_ids_tests.cpp`) pins assignment, the manifest and the error cases.
`ir_ids_stability` (`tests/unit/ir_ids_stability_tests.cpp`) pins the section 7 contract: inserting
`X` before/after `A, B, C` keeps their component and node ids; same-named components get distinct ids
and keep them when a differently named component is inserted; reordering them exchanges ids; a
duplicated `componentId` is still `TESSERA_IR_DUPLICATE_ID` and a duplicated explicit
`automationId` is still a hard assignment error; and inserting a component into the library TSX
keeps the generated C++ of `Field`/`Form` byte-identical while the committed library IR still
matches a fresh lowering.

Still required for the full section (not produced by this slice):

- The headless harness loading the manifest to resolve ids to names and sources.
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
- **ID stability across hot reload** — IDs are pure functions of `(salt, source path, component
  name, occurrence, node path, key)`, so unchanged components and nodes keep their IDs; changed
  subtrees re-ID and the manifest diff shows it (section 7).
- **Encoding for very large snapshots** — canonical JSON with uint64 decimal strings; very large
  trees stream NDJSON per node; zstd is opt-in only after a >16 MiB measurement.

The script lane loads the manifest (see [`headless-automation-v1.md`](headless-automation-v1.md)
section 2); the stdio serve lane does not yet. Not yet implemented: `windowId` in the snapshot
header, `partId` fields, NDJSON streaming, and zstd.
