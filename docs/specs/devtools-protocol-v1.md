# Native devtools protocol v1

Status: proposed (Big Steps C and D, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision: [ADR-031](../../decisions/ADR-031-production-tooling-and-distribution.md) (devtools, updates/signing, crash reporting). Companion to
[`diagnostics.md`](../design/diagnostics.md), [`designer.md`](../design/designer.md),
[`automation-ids-v1.md`](automation-ids-v1.md), and [`headless-automation-v1.md`](headless-automation-v1.md).

A local, opt-in protocol for inspecting and debugging a running native app. It is not a JS
console: Tessera has no production JS runtime.

## 1. Goals

- Inspect scene/semantics trees, computed style, layout boxes, draw calls, dirty reasons (why a node was flagged for recomputation, [`dirty-rendering.md`](../design/dirty-rendering.md)).
- Record/replay input, trace spans, metrics, screenshots, and custom element snapshots ([`custom-elements-v1.md`](custom-elements-v1.md)).
- Hot reload of generated components/styles (state preservation is best-effort, section 4).
- Local-only, authenticated, bounded, and disabled in release by default.

## 2. Transport

- Line-delimited JSON over stdin/stdout (CLI) or a local socket; no network listener.
- Request/response with `id`; events pushed as `event`.
- Max message size, timeout, and backpressure; invalid messages close only the session.
- IDs (`uiId`, `nodeId`, `componentId`, `traceId`; defined in [`automation-ids-v1.md`](automation-ids-v1.md) section 1) are JSON decimal strings, per [`automation-ids-v1.md`](automation-ids-v1.md).
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
- `custom` is debug/headless-only and maps to `on_test_command` (a debug/headless-only hook, [`custom-elements-v1.md`](custom-elements-v1.md)) where allowed (the rule is not specified).

## 4. Hot reload

```text
file change -> hash -> incremental compile -> generated patch
  -> apply/reconcile -> preserve state by component/key where safe (the safety rule is not specified; full reload is the fallback)
  -> request one frame
```

- Style-only changes do not rebuild the tree; structural changes reconcile in place.
- State preservation is best-effort by component/key; full reload is the fallback.
- Removed nodes/GPU resources retire after frames in flight (frames already submitted to the GPU and not yet finished).
- A failed compile keeps the last good frame and returns diagnostics.
- Hot reload is dev-only; release builds do not include the watcher/compiler. The rest of the
  devtools surface is disabled in release by default (section 6).

## 5. Inspector overlay

- Toggleable dev overlay; node hover/click selects tree/style/layout/semantics.
- Shows dirty domains/reasons, draw calls/batches, frame stages, memory/cache, recent events.
- Click-to-source maps generated/scene nodes to TSX/C++ source spans via the generated manifests (automation manifest, [`automation-ids-v1.md`](automation-ids-v1.md) section 2, and the binding manifest, [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) (protected regions and binding manifest)).
- Overlay never defeats dirty rendering (only its own region or explicit debug mode).
- No secrets/PII; redaction applies to screenshots/traces/events.

## 6. Security

- Local-only; no remote binding; auth token required if a socket is used.
- Validate commands/paths/ids; reject traversal/oversized payloads.
- Inspection is read-only by default; mutation commands are explicit and dev-only.
- Hot reload executes only compiler output, never project JS at runtime.
- The devtools surface is disabled in release by default ([ADR-031](../../decisions/ADR-031-production-tooling-and-distribution.md)); devtools code is compiled into a release build only when the CMake option `TESSERA_ENABLE_DEVTOOLS` is `ON` (default `OFF`; this option does not exist yet and is added when devtools is implemented). A build made without that option has no runtime switch that enables devtools.

## 7. Testing

- Protocol framing/versioning/malformed/oversized messages.
- Tree/style/layout/semantics snapshot schemas; ID correlation.
- Event subscription/filter/backpressure.
- Screenshot/trace path validation; redaction.
- Hot reload style-only/structural/failed compile/state preservation.
- Devtools disabled in release; no measurable cost when off.

## 7.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `DevtoolsServer` | local transport, auth/session, framing | App state |
| `DevtoolsSession` | command dispatch, subscriptions, backpressure | UI thread policy |
| Snapshot builders | tree/style/layout/semantics/metrics views | Mutating app state |
| HotReloader | compile/patch/reconcile, state preservation | General build system |
| Headless harness ([`headless-automation-v1.md`](headless-automation-v1.md)) | scripts/asserts/traces | Devtools transport |

Protocol:

```json
{"id": 1, "cmd": "tree", "params": {"root": "..."}}
{"id": 1, "ok": true, "result": {...}}
{"event": "frame", "data": {...}}
```

- `protocolVersion` (see [`versioning-v1.md`](versioning-v1.md) section 2) is exchanged on connect; unknown major -> reject.
- Request ids are client-scoped; responses/events are bounded and ordered per session.
- Snapshot schemas are versioned independently; IDs are strings in JSON.
- Backpressure: bounded event queue, drop oldest for high-frequency events, never for command
  responses; slow clients are disconnected.
- Local-only transport: stdio or a per-user socket; no TCP listener/remote bind.

Hot reload:

```text
watch -> hash -> incremental compile -> validate patch
  -> apply/reconcile (style-only or structural)
  -> preserve state by (component,key) where safe (rule not specified; full reload is the fallback)
  -> request one frame
  -> failed compile: keep last good frame + diagnostics
```

- Style-only changes skip tree rebuild; structural changes reconcile by keyed identity.
- State preservation is best-effort; a full reload is the explicit fallback.
- Removed nodes/resources retire after frames in flight.
- Hot reload code is excluded from release builds; other devtools code is hard-disabled in release by default (see section 6).

Security:

- Session token/peer credentials; commands validated; paths allowlisted to project/artifacts.
- Read-only by default; mutation/test commands require an explicit dev flag.
- Screenshots/traces/events are redacted; secure fields never included.
- A disconnected client cannot leave subscriptions/callbacks behind.

Failure/edge cases:

- Invalid/oversized message closes the session, not the app.
- Client disconnect during a command -> cancel/discard; no UI mutation.
- Stale node/id after reload -> return `not_found`, not a crash.
- Hot reload failure keeps the last good frame; never a white screen.
- Multiple clients: one primary inspector session; a connection from an additional client is rejected with an error and does not affect the primary session.

Gap-closure decisions (ids G-n are gaps and Q-n are open questions, listed in [`app-completeness-cross-check-v1.md`](../architecture/app-completeness-cross-check-v1.md); resolutions in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-22/Q-21**: protocol v1 is JSON-lines over stdio or a per-user socket with a session token.
  Hot reload has two patch kinds: style-only patch and structural reconcile; state is preserved
  by `(component,key)` where safe (the safety rule is not specified), otherwise full reload. Failed compile keeps the last good
  frame. One primary client; a connection from an additional client is rejected with an error and does not affect the primary session. Remote debugging is
  forbidden.

## 8. Deferred/open questions

- Time-travel/recording format.
- IDE/editor protocol integration.
- Multi-client collaboration beyond one primary session.
- Structural patch format details for nested control flow (reconcile fallback).
