# Navigation and routing v1

Status: proposed (Big Step F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big Steps").
Decision: [ADR-030](../../decisions/ADR-030-app-platform-completeness.md) (app-platform
completeness: navigation, restore, shell, i18n, accessibility, data). Companion to
[`deep-linking-v1.md`](deep-linking-v1.md), [`startup-and-loading.md`](../design/startup-and-loading.md),
[`state-restoration-v1.md`](state-restoration-v1.md), and [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md).

Navigation is declarative and native: routes, a back stack, guards, and state restoration, with
no browser/JS router runtime.

## 1. Route table

```json
{
  "routes": [
    { "id": "home", "path": "/", "screen": "Home" },
    { "id": "orders", "path": "/orders", "screen": "Orders" },
    { "id": "orders.detail", "path": "/orders/:id", "screen": "OrderDetail", "auth": true },
    { "id": "settings", "path": "/settings", "screen": "Settings" }
  ],
  "notFound": "not_found"
}
```

- Routes are static/declarative; `id` is stable, `path` is for matching/deep links.
- Path patterns support literals, `:param`, optional `?`, and `*` wildcard; no arbitrary regex.
- Params are typed/validated; unknown/malformed params -> not-found or guard redirect.
- Nested routes/outlets are supported via layout routes; modal routes are explicit.
- Route table is validated at build time; duplicate ids/paths are errors.
- `screen` resolves to a component; no dynamic component expression.

## 2. Stack model

- `push`, `pop`, `replace`, `reset`, `pop_to`, `replace_all`.
- A stack entry has `route_id`, params, per-route state, and a stable entry id.
- Back = pop unless the route/overlay handles it; Android back, browser-style back, and
  keyboard shortcuts map to the same operation.
- Replace does not grow history; reset clears it; pop_to removes entries above the target.
- Duplicate policy: push same route with same params -> focus/no-op or replace, explicit per
  route.
- Back at root: exit/close per platform policy; never silently quit without confirmation when
  unsaved work exists.

## 3. Matching and params

```cpp
RouteMatch match(std::string_view path);
// { route_id, params, query, matched_path, rest }
```

- Decode/normalize once; reject malformed UTF-8/percent-encoding.
- Typed params (`int`, `uuid`, `enum`, `string`) validated before screen mount.
- Query/state is not a substitute for typed params; document precedence.
- Route changes are frame-boundary operations; no navigation in render.
- Unknown route -> not-found screen with back/home actions.
- Route load can be async (data); show loading/error per route, not a white screen.

## 4. Guards and auth

- Guards run before commit: `allow`, `redirect`, `deny`, `login`.
- Auth-gated routes wait for auth state; deep links preserve the intended destination.
- Redirects are bounded (no loops; max 8, see G-12 in section 10.1); each guard has a timeout
  (default 5 s, G-12).
- Unsaved-changes guard asks before pop/replace/exit.
- Permission/role checks are explicit; denial shows a safe screen, not a crash.
- Async guards are cancellable; stale results are discarded by navigation epoch.

## 5. Deep links and startup

- A deep link resolves to a route + params, then uses the same push/replace path.
- Cold start: wait for app/route/auth readiness; preserve the pending link; fallback URL for web.
- Warm start: focus the existing window and navigate if allowed; single-instance rules apply.
- Invalid/expired/unauthorized link -> safe fallback screen; never navigate to a half state.
- Startup restore uses the saved stack only if the route table/app version still matches.
- See [`deep-linking-v1.md`](deep-linking-v1.md) and
  [`state-restoration-v1.md`](state-restoration-v1.md).

## 6. Modal routes and overlays

- A modal route is a stack entry with `presentation: modal|sheet|fullscreen`.
- Dismissal pops the route; focus/back/escape follow the overlay spec.
- A modal route does not mutate the parent route's state; it has its own entry state.
- Nested navigation inside a modal shares the global stack when `navigation.modalStack` is
  `global` (the default, Q-11 in section 10.1) or gets its own modal-local stack when it is
  `nested` ([`project-config-v1.md`](project-config-v1.md)). `nested` means modal stack nesting
  only; it does not mean per-tab stacks.
- Toasts/tooltips are overlays, not routes.

## 7. Transitions

- Route transitions are animations; they do not change navigation semantics.
- Push/pop/replace choose a transition; reduced motion skips it.
- Back during a transition cancels/reverses safely; no double commit.
- Async route load can show a skeleton; no fake progress.
- Transition completion is deterministic; tests use a fixed clock.

## 8. State and restoration

- Each stack entry may persist typed state (scroll, filters, selection, drafts); the policy
  that selects what is persisted is not specified here (see
  [`state-restoration-v1.md`](state-restoration-v1.md)).
- Restoration is versioned; unknown/old state falls back safely.
- Multi-window: each window has its own stack; a deep link targets a window (targeting policy: open question in section 11).
- Logout clears user-scoped routes/state; auth screens are not restored as protected content.
- See [`state-restoration-v1.md`](state-restoration-v1.md).

## 9. Testing

- Match/params/query/encoding, duplicate routes, unknown route.
- Push/pop/replace/reset/pop_to, duplicate policy, back at root.
- Guards, redirects, auth, unsaved changes, cancellation/epoch.
- Deep link cold/warm/malformed/auth-gated.
- Modal route dismissal/focus/back.
- State restore round-trip/version mismatch.
- Headless scripts: navigate/back/deep-link/assert route/state.

## 10. Security

- All deep-link/route params are untrusted: decode/validate, length limits, no traversal.
- No route to arbitrary component code; screen ids are compile-time.
- Do not include tokens/secrets in paths; redact logs.
- Auth guards are enforced before mounting protected screens/data.

## 10.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `RouteTable` | static routes, patterns, params, guards metadata | Screen state/data |
| `NavController` | active stack, push/pop/replace, epoch, transitions | Overlay/modal dismissal policy |
| `NavEntry` | route id, params, per-route state, presentation, stable entry id | Data cache |
| `GuardPipeline` | auth/permission/unsaved checks, redirects, timeout | Auth implementation |
| OverlayService (see [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md)) | non-route overlays/tooltips/toasts | Route stack |

`NavEntry` data:

```cpp
struct NavEntry {
    std::uint64_t entry_id;     // stable for restore/automation
    std::string route_id;       // canonical, not a path
    ParamMap params;            // typed/validated
    StateMap state;             // serializable, opt-in restore
    Presentation presentation;  // page/modal/sheet/fullscreen
    TransitionSpec transition;
};
```

State machine:

```text
Idle -> Resolving -> Guarding -> Committing -> Transitioning -> Active
Active -> Popping -> Active/Idle
Any -> Cancelled (newer epoch)
```

- Every navigation increments `navigation_epoch`; async guards/data results from an older epoch
  are discarded.
- Only one navigation commits at a time; a second request cancels or queues (the policy that
  chooses is not specified).
- During a transition, back requests target the transition's source/destination deterministically;
  no double commit.

Back priority (canonical):

1. Topmost dismissible overlay (see overlay spec).
2. Topmost modal/sheet navigation entry.
3. Normal route stack pop.
4. Root/OS exit policy (unsaved-work guard first).

Guard pipeline:

```text
resolve params -> auth state -> guards (ordered)
  -> allow -> commit
  -> redirect -> resolve target (max N = 8 per G-12, loop detection)
  -> deny/login -> safe screen
```

- Each guard has a timeout; cancellation is not an error.
- Unsaved-changes guard returns `ask`, `save`, `discard`, `cancel`; no silent discard.
- Redirects preserve the intended destination only if the guard allows it (no policy format is
  specified); no open redirect.
- Guards cannot mutate the route table or mount screens.

Route data:

- A route may declare `loader` (static binding) and `prefetch`.
- Loader runs after params are validated and before/when the screen mounts; loader epoch is the
  navigation epoch + route entry id.
- Loading/error/empty states are route-level and cancellable on pop/replace.
- Cached data is owned by `async-data` ([`async-data-v1.md`](async-data-v1.md)), not the navigation stack. Route state stores only
  serializable UI state (tab/filter/scroll key).

Deep links/restore:

- A deep link resolves to `(route_id, params)` then uses the same commit path; it never bypasses
  guards.
- Cold start waits for auth readiness; pending activation is bounded.
- Restore builds a stack of `NavEntry` with validated route ids/params; invalid entries are
  dropped or replaced by the route root.
- `navigation.restore` controls stack restore; `restore.enabled` is the master;
  `loading.restoreRoute` only resumes the pending activation after auth.

Failure/edge cases:

- Invalid/malformed params -> not-found/fallback; no half-mounted screen.
- Redirect loop -> stop after N, diagnostic, safe screen.
- Screen build/effect error -> error boundary fallback, stack entry remains poppable.
- Rapid push/pop -> epoch cancellation; no stale transition completion.
- Multi-window: each window owns its stack; deep-link targeting is explicit.

Gap-closure decisions (`G-` and `Q-` IDs refer to
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-12**: guards return `Allow/Redirect/Deny/Login`, run with an epoch/cancel, default 5 s
  timeout, max 8 redirects, and one commit at a time. Back during a transition targets the
  transition destination; a second request cancels or queues (policy not specified).
- **G-13**: route loaders start after guards and may mount a skeleton; results live in
  `async-data`, keyed by `(route_id, entry_id, params)`. Prefetch is intent-based. The navigation
  stack stores only serializable UI state, never data payloads.
- **Q-11**: one global stack by default; modal/sheet entries are stack entries with
  `presentation`; per-tab nested stacks are not in V1 (the `modalStack: nested` config value is
  modal-local nesting, not per-tab stacks).
- **Q-12**: route loader -> async-data boundary is fixed as above; route cache is not a second
  data cache.

## 11. Deferred/open questions

- Browser-history integration for desktop/web preview.
- Multi-window/deep-link targeting policy.
- Type generation for params from the route table.
- Per-tab nested stacks (not in V1; no later release is assigned).

V1 is the first-release scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8). Other items above have no decision yet.
