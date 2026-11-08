# Navigation and routing v1

Status: proposed (Big Step F). Decision: ADR-030. Companion to
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
- Redirects are bounded (no loops); each guard has a timeout.
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
- Nested navigation inside a modal has its own stack or shares the global stack by policy.
- Toasts/tooltips are overlays, not routes.

## 7. Transitions

- Route transitions are animations; they do not change navigation semantics.
- Push/pop/replace choose a transition; reduced motion skips it.
- Back during a transition cancels/reverses safely; no double commit.
- Async route load can show a skeleton; no fake progress.
- Transition completion is deterministic; tests use a fixed clock.

## 8. State and restoration

- Each stack entry may persist typed state (scroll, filters, selection, drafts) by policy.
- Restoration is versioned; unknown/old state falls back safely.
- Multi-window: each window has its own stack; a deep link targets a window by policy.
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

## 11. Open questions

- Nested stack vs flat stack for modals.
- Browser history integration for desktop/web preview.
- Route-level data loading/caching contract.
- Multi-window link targeting.
- Type generation for params from route table.
