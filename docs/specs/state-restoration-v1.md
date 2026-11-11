# State restoration v1

Status: proposed (Big Step F, production breadth; see [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision:
[ADR-030](../../decisions/ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n,
accessibility, async data). Companion to
[`navigation-v1.md`](navigation-v1.md), [`startup-and-loading.md`](../design/startup-and-loading.md),
[`mobile-integration.md`](../design/mobile-integration.md), and
[`services.md`](../design/services.md).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8).
**uiId** is the numeric automation/UI id of a node ([`automation-ids-v1.md`](automation-ids-v1.md)).
**ItemKey** is the stable key of a virtualized list item ([`virtualization-v1.md`](virtualization-v1.md)).
**Keystore** is the platform secret store. **fsync** flushes a file to disk. Gap/question ids
(`G-14`, `Q-13`) refer to rows in
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md).

Restoration is not “save the whole heap”. It is an explicit, versioned, bounded snapshot of
user-visible continuity.

## 1. Triggers

- App background/suspend, process death, fold/unfold restart, window close/reopen.
- Crash recovery (best effort; never trust a corrupt snapshot).
- Manual session restore/relaunch.
- Multi-window: per-window state plus app-global state.

The launch modes that read this snapshot (process restore, warm start), their reuse rules and their
precedence over a pending deep-link/notification activation are defined in
[`startup-api-v1.md`](startup-api-v1.md) section 5.1; this document owns the snapshot itself.

## 2. What to persist

| State | Default | Notes |
|---|---|---|
| Route stack + params | yes | validated against current route table |
| Per-route state | opt-in | typed, versioned, bounded |
| Window geometry/display | yes | clamp to current displays |
| Scroll offsets | opt-in | keyed by route/list id |
| Focus/selection | opt-in | stabilize if nodes still exist |
| Form drafts | opt-in | secure fields excluded |
| Text field value | opt-in | secure/undo history excluded |
| Overlay stack | transient | closed by default; a modal route is restored only under the rule in section 7.1 (gap-closure decisions) |
| Async cache | no | separate data cache, see [`async-data-v1.md`](async-data-v1.md) |
| Diagnostics/telemetry | no | never restore |

- Persist only serializable, app-owned state; no pointers/handles/GPU objects.
- Large documents use explicit document storage, not the restoration blob.
- Unsaved-work state must be recoverable or clearly discarded with a prompt.
- Secrets/tokens go to the keystore, never the restoration blob.

## 3. Snapshot format

```json
{
  "restoreVersion": 1,
  "appVersion": "1.4.0",
  "savedAt": "2026-11-07T10:00:00Z",
  "windows": [
    {
      "id": "main",
      "bounds": { "x": 100, "y": 80, "w": 1280, "h": 800 },
      "display": 1,
      "routeStack": [
        { "route": "home", "params": {}, "state": {} },
        { "route": "orders.detail", "params": { "id": "42" }, "state": { "tab": "items" } }
      ],
      "scroll": { "orders.list": 320 },
      "focus": { "route": "orders.detail", "uiId": "123" }
    }
  ]
}
```

- Typed, versioned, canonical JSON (a bounded binary encoding is not specified for V1; G-14/Q-13 below choose JSON).
- `restoreVersion` migrates forward; unknown fields are ignored safely.
- Size limits per window/state; oversized state is dropped with a diagnostic.
- Write atomically (temp + rename); keep one previous good snapshot.

## 4. Restore order

1. Load/validate snapshot (schema/version/limits).
2. Recreate window(s) with clamped bounds.
3. Apply app/global services state (settings/auth) before routes.
4. Restore route stack, dropping invalid routes/params.
5. Mount screens; apply scroll/focus/selection after layout.
6. Apply form drafts only after fields exist; discard secure fields.
7. Restore overlays only under the section 7.1 modal rule (default: none).
8. Mark dirty/request one frame; never block startup on non-critical state.

- A failed step falls back to the route root, not an empty/white app.
- Auth-gated routes wait for auth; if unauthenticated, restore after login or show login first.
- Unknown app version/route table -> safe fallback + diagnostic; no crash.

## 5. Multi-window and process death

- Each window has a stable id; a deep link targets a window by the policy in [`deep-linking-v1.md`](deep-linking-v1.md).
- Closing one window persists its state; reopening restores it unless the app chose one-shot.
- Process death may lose in-memory async work; show recoverable draft/error states.
- Fold/unfold restart uses the same snapshot; preserve scroll/selection per pane.
- Do not restore two windows onto the same display rect without clamping/cascade.

## 6. Security and privacy

- Do not restore secrets/tokens; read them from the keystore.
- Do not restore secure text values, clipboard contents, or undo history by default.
- Redact paths/user data in diagnostics; snapshots are app-private.
- Validate all restored ids/params/paths; never trust the blob.
- Logout clears user-scoped snapshots; shared-device safety.
- Corrupt/partial snapshots are discarded, never executed.

## 7. Testing

- Round-trip route stack/params/state; version migration; unknown fields.
- Window bounds across display changes/minimized/fullscreen.
- Scroll/focus/selection restore after layout; missing nodes.
- Form draft restore; secure fields excluded.
- Process death during write -> previous good snapshot.
- Multi-window/fold/display removal.
- Headless restore scripts; deterministic time/geometry.

## 7.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `RestoreService` | snapshot schema, write/read, migration, limits | Route stack semantics |
| `NavController` | route stack/entry state | Persistence format |
| Screen/component | serializable opt-in state | Secret storage |
| `services/storage` | atomic file/path/encryption primitives | What to restore |
| `async-data` ([`async-data-v1.md`](async-data-v1.md)) | data cache | Restoration blob |

Snapshot types:

```cpp
struct RestoreSnapshot {
    std::uint32_t restore_version = 1;
    std::string app_version;
    std::int64_t saved_at_ns;
    std::vector<WindowSnapshot> windows;
};

struct WindowSnapshot {
    std::string window_id;          // stable logical id
    Rect bounds;                    // logical px
    std::string display_id;         // optional; re-resolve if gone
    std::vector<RouteEntrySnapshot> route_stack;
    std::map<std::string, double> scroll;   // stable key -> offset
    std::optional<FocusSnapshot> focus;
    std::map<std::string, DraftSnapshot> drafts;
};
```

Write policy:

- Mark dirty on meaningful state change; debounce (1 s, per G-14/Q-13 below) and force on
  background/suspend/window close.
- Write temp file + fsync + rename; keep one previous good snapshot.
- Enforce per-window/total byte limits; drop low-priority state (scroll/drafts) before routes.
- Never write on every keystroke/scroll tick; no blocking I/O on the UI thread.
- A write failure is diagnostic-only; never crash/exit.

Restore order (per window):

```text
validate schema/version/limits
  -> clamp window bounds to current displays
  -> wait for app services/auth readiness
  -> rebuild route stack (drop invalid routes/params)
  -> mount screens
  -> after first layout: scroll -> focus/selection -> drafts
  -> restore overlays per the modal rule below (default none)
  -> request one frame
```

- Route entry ids are regenerated deterministically; per-route state keys remain stable.
- Scroll keys: `route_id + container_ui_id + item_key` (not pixel/index only).
- Focus is restored by `ui_id`/semantic key only if the node exists; otherwise ignore.
- Drafts are restored only after fields exist and never for secure fields.
- Auth-gated entries wait for auth; if unauthenticated, keep pending/last safe route.

Migration/versioning:

- `restore_version` migrates forward; unknown fields ignored, unknown required fields fail safe.
- App/route-table version mismatch: drop incompatible entries, keep a safe root.
- A migration never executes restored code or arbitrary paths.

Failure/edge cases:

- Corrupt/partial snapshot -> previous good or safe root; diagnostic.
- Oversized state -> drop optional sections, keep route stack; report.
- Display removed -> clamp/cascade on primary work area.
- Process death during write -> previous snapshot remains valid.
- Missing route/screen/scroll key -> ignore that fragment, keep the rest.
- Logout/tenant switch clears user-scoped snapshots before writing new state.

Gap-closure decisions:

- **G-14/Q-13**: canonical JSON snapshot v1, <=256 KiB/window; debounce 1 s and force on
  background/suspend/window close; temp+fsync+rename with one previous good snapshot. Keys are
  `route_id` + numeric `uiId` + `ItemKey`; no `NodeId`. Migrations are explicit and forward-only;
  sensitive drafts may be encrypted, but secure fields are never persisted.
- Virtualized scroll restores by anchor key (first visible stable item + delta), not raw index.
- Overlay/modal stack is not restored by default; a modal route is restored only if its parent
  route and auth state are valid.

## 8. Deferred/open questions

- Cross-device/cloud session restore (out of scope for V1).
- Per-window vs per-app draft merge policy (no decision yet).
- Snapshot format compaction/encryption key management (no decision yet; G-14/Q-13 only decide
  that sensitive drafts may be encrypted).
- Restoring a modal route whose parent route changed (no decision yet).
