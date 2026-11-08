# Native devtools protocol v1

Status: proposed (Big Step C/D). Decision: ADR-031. Companion to
[`diagnostics.md`](../design/diagnostics.md), [`designer.md`](../design/designer.md),
[`automation-ids-v1.md`](automation-ids-v1.md), and [`headless-automation-v1.md`](headless-automation-v1.md).

A local, opt-in protocol for inspecting and debugging a running native app. It is not a JS
console: Tessera has no production JS runtime.

## 1. Goals

- Inspect scene/semantics trees, computed style, layout boxes, draw calls, dirty reasons.
- Record/replay input, trace spans, metrics, screenshots, and custom element snapshots.
- Hot reload of generated components/styles where safe.
- Local-only, authenticated, bounded, and disabled in release by default.

## 2. Transport

- Line-delimited JSON over stdin/stdout (CLI) or a local socket; no network listener.
- Request/response with `id`; events pushed as `event`.
- Max message size, timeout, and backpressure; invalid messages close only the session.
- IDs (`uiId`, `nodeId`, `componentId`, `traceId`) are decimal/hex strings.
- Optional token from the app's dev session; no remote access.

## 3. Commands

```json
{"id":1,"cmd":"tree"}
{"id":2,"cmd":"style","nodeId":"42"}
{"id":3,"cmd":"layout","uiId":"987654321098765432"}
{"id":4,"cmd":"screenshot","path":"out/frame.png"}
{"id":5,"cmd":"trace","action":"start","path":"out/trace.json"}
{"id":6,"cmd":"reload","changed":["ui/App.tsx"]}
{"id":7,"cmd":"custom","uiId":"...","command":"set_camera","args":{"yaw":1.2}}
```

Commands: `tree`, `node`, `style`, `layout`, `semantics`, `events`, `metrics`, `screenshot`,
`trace`, `capture`, `reload`, `custom`, `settings`, `close`.

- All reads are immutable snapshots; no UI mutation from inspection.
- `events` can subscribe with filters (category/uiId/type); bounded ring/buffer.
- `reload` applies a patch or falls back to full reload; failed compile keeps the last frame.
- `custom` is debug/headless-only and maps to `on_test_command` where allowed.

## 4. Hot reload

```text
file change -> hash -> incremental compile -> generated patch
  -> apply/reconcile -> preserve state by component/key where safe
  -> request one frame
```

- Style-only changes do not rebuild the tree; structural changes reconcile in place.
- State preservation is best-effort by component/key; full reload is the fallback.
- Removed nodes/GPU resources retire after frames in flight.
- A failed compile keeps the last good frame and returns diagnostics.
- Hot reload is dev-only; release builds do not include the watcher/compiler.

## 5. Inspector overlay

- Toggleable dev overlay; node hover/click selects tree/style/layout/semantics.
- Shows dirty domains/reasons, draw calls/batches, frame stages, memory/cache, recent events.
- Click-to-source maps generated/scene nodes to TSX/C++ source spans via manifests.
- Overlay never defeats dirty rendering (only its own region or explicit debug mode).
- No secrets/PII; redaction applies to screenshots/traces/events.

## 6. Security

- Local-only; no remote binding; auth token required if a socket is used.
- Validate commands/paths/ids; reject traversal/oversized payloads.
- Inspection is read-only by default; mutation commands are explicit and dev-only.
- Hot reload executes only compiler output, never project JS at runtime.
- Disable the entire devtools surface in release unless explicitly enabled.

## 7. Testing

- Protocol framing/versioning/malformed/oversized messages.
- Tree/style/layout/semantics snapshot schemas; ID correlation.
- Event subscription/filter/backpressure.
- Screenshot/trace path validation; redaction.
- Hot reload style-only/structural/failed compile/state preservation.
- Devtools disabled in release; no measurable cost when off.

## 8. Open questions

- Socket transport/auth and multi-client sessions.
- Diff/patch format for hot reload.
- Time-travel/recording format.
- Integration with IDEs/editor protocol.
- Remote debugging policy (likely forbidden).
