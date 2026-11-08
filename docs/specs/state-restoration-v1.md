# State restoration v1

Status: proposed (Big Step F). Decision: ADR-030. Companion to
[`navigation-v1.md`](navigation-v1.md), [`startup-and-loading.md`](../design/startup-and-loading.md),
[`mobile-integration.md`](../design/mobile-integration.md), and
[`services.md`](../design/services.md).

Restoration is not “save the whole heap”. It is an explicit, versioned, bounded snapshot of
user-visible continuity.

## 1. Triggers

- App background/suspend, process death, fold/unfold restart, window close/reopen.
- Crash recovery (best effort; never trust a corrupt snapshot).
- Manual session restore/relaunch.
- Multi-window: per-window state plus app-global state.

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
| Overlay stack | transient | close by default; modals by policy |
| Async cache | no | separate data cache, see async data |
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

- Typed, versioned, canonical JSON (or a bounded binary encoding later).
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
7. Restore overlays by policy (default: none).
8. Mark dirty/request one frame; never block startup on non-critical state.

- A failed step falls back to the route root, not an empty/white app.
- Auth-gated routes wait for auth; if unauthenticated, restore after login or show login first.
- Unknown app version/route table -> safe fallback + diagnostic; no crash.

## 5. Multi-window and process death

- Each window has a stable id; a deep link targets a window by policy.
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

## 8. Open questions

- Binary vs JSON encoding and size budgets.
- Per-route state schema generation.
- Draft autosave cadence vs battery/IO.
- Cross-device/session restore (cloud) — likely out of scope.
- Overlay/modal restore policy.
- Restoration of virtualized scroll positions.
