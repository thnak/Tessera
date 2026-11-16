# Navigation

Part of the [services design set](README.md). Section 3 is the navigation target and 3.1 the
module that shipped (`tessera_appfw_navigation`, issue #196).

## 3. Navigation

- Routes are declarative; a stack model with push/pop/replace/reset.
- Deep links map to routes with validated parameters; see
  [`deep-linking-v1.md`](../../specs/deep-linking-v1.md).
- Back handling is platform-aware (Android back, browser history, keyboard shortcuts).
- State restoration: save/restore route stack and per-route state.
- Transitions are animations, not navigation semantics; reduced motion skips them.
- Modal/dialog navigation has an explicit dismiss policy.
- Tests: route matching, params, back stack, deep links, restore, unknown routes.

### 3.1 Implemented shape (issue #196)

The `navigation` module ships as `tessera_appfw_navigation` (ADR-050 section 1 layer 1) in five pieces:
`RouteTable` (patterns, typed params, fingerprint), `NavController` (the stack, the epoch, the
frame-boundary route), `install_deep_link`, the restore `NavSnapshot` codec and `NavRestoreStore`. It is
the smallest slice that satisfies the issue's acceptance - *"route/back/deep-link/restore tests per
`docs/specs/navigation-v1.md`"* - and the rest of that spec is named below rather than half-built.

**The route is an AF4 `Observable<RouteSnapshot>`, and a request is not a commit.** Section 3 says
"route changes are frame-boundary operations; no navigation in render", so the five operations mutate
the stack and mark it dirty; `commit()` publishes the new active entry. Three requests in one tick
produce one notification with the last value, and a tick that ends on the route it started with wakes
nobody. The epoch is *not* part of `RouteSnapshot::operator==`: it exists for section 4's "stale
results are discarded by navigation epoch" and must not make a redundant request look like a change.

**Back is a value, not a side effect.** Section 10.1's priority is overlay -> modal/sheet -> stack pop
-> OS exit. Overlays belong to `ui::scene::OverlayHost` (overlay spec section 8.1), so this module owns
steps 2 to 4 and reports which it took (`DismissedModal`, `Popped`, `AtRoot`). It never closes a window
or quits: section 2's exit policy, including "never silently quit without confirmation when unsaved
work exists", is a host decision that a module which quit would make impossible. **Back with an empty
stack is `Empty`** - reachable only before the first commit of a controller built without an initial
route - and it changes nothing.

**The route table tie-break is a rule, not the table order.** Section 1 makes duplicate *paths* an
error, but `/orders/new` and `/orders/:id` both match that text. The matcher prefers the most literal
segments, then the fewest wildcards, then the fewest params, then the earliest row, so a literal route
is reachable without relying on where an author put it. `:name?` is only legal as the final segment, and
`*` is a whole trailing segment that also matches the empty remainder, which is what makes `/files`
work. **Untrusted input is refused in classes, never thrown.** Section 10 and
[`deep-linking-v1.md`](../../specs/deep-linking-v1.md) section 7 name the classes, and each has its own
`NavigationErrorCode`: `InvalidPath`, `MalformedPercentEncoding`, `EncodedSeparator` (a decoded `/` or
`\` would re-segment a matched path), `ControlCharacter`, `InvalidUtf8`, `PathTooLong` (4096 bytes,
`deepLinks.maxUrlLength`'s default, checked before the walk). An unknown route is `NotFound`, a named
failure the caller maps to `RouteTable::not_found_route()`: section 3 says the unknown route shows the
not-found screen, and which screen that is is table data.

**A cold deep link synthesizes a root.** Section 5 says a cold start preserves the pending link and
section 10.1 says a deep link "uses the same commit path", but neither says what the stack *is* when a
link arrives with nothing under it - and a single-entry stack would make the first `back()` an `AtRoot`
edge, trapping the user on the screen the link opened. So `install` seeds `initial_route()` and then
pushes the target (`Synthesized`), skipping the seed when the link targets the root. A warm stack is
pushed onto unchanged (deep-linking-v1.md section 2). **Restore persists the stack and nothing else,
through `Storage`.** state-restoration-v1.md section 2's
row "Route stack + params" is `yes` and every other row is opt-in, so the snapshot carries the stack,
its `state` and the active index. The format is canonical JSON at `restoreVersion` 1, capped at 256 KiB
per window (G-14/Q-13), written through AF7's `Storage`, so every save is the same
temp-write/flush/atomic-replace sequence the app's other data uses and every byte leaves the UI thread
through an `IoTaskExecutor`. The store owns no debounce: section 7.1's 1 s rule needs a clock, which a
module whose tests are deterministic by construction cannot have.

**Four gates refuse a snapshot, and each has a name**, in section 4's order: size before parsing
(`TooLarge`), schema and `restoreVersion` (`MalformedJson`/`MalformedSnapshot`/`UnsupportedVersion`),
per-entry pruning, then identity. Identity is **the route-table fingerprint** (FNV-1a over the canonical
`id, path` rows): section 5 makes "the route table still matches" a condition for restoring and section
7.1's snapshot has no member for it. The app version is compared only when
`RestorePolicy::expected_app_version` is set, because section 3 says `restoreVersion` migrates forward
and a patch release must not lose the user's stack. **A flow that no longer exists is dropped, not
restored and not fatal.** navigation-v1.md section 10.1
("invalid entries are dropped or replaced by the route root") and state-restoration-v1.md section 7.1
("drop incompatible entries, keep a safe root") are one rule, applied twice because the fingerprint
cannot catch an id rename in an otherwise identical table: `decode_nav_snapshot` prunes an entry whose
route id the table lost (counting it in `dropped`), and `replace_all` revalidates every entry, so a
hand-built stack is checked by the same code a navigation is. The rest still restores.

**What this slice deliberately does not do**, and what each piece needs (the next slice, not a guess):
**Guards and auth** (section 4): `RouteDef::auth` is carried and `DeepLinkResult::requires_auth`
reports it, but no `GuardPipeline` runs. Mounting a protected screen would violate section 10's "auth
guards are enforced before mounting protected screens/data", and a pending queue would invent the
policy [`deep-linking-v1.md`](../../specs/deep-linking-v1.md) section 10 leaves open. **Route loaders and
`async-data`** (section 10.1 "Route data", G-13) are absent for the same kind of reason, and
`NavEntry.state` is UI state only: a data payload in it would be the second data cache Q-12 forbids.
**Transitions** (section 7: "they do not change navigation semantics") - `Presentation` is carried for
section 10.1's back priority, not for an animation. **Layout routes and nested outlets** (section 1),
**per-tab stacks** (Q-11: not in V1, though the encoded form already has `stacks: [{id, entries}]`),
**multi-window targeting** (section 11's open question) and **encryption at rest**
(state-restoration-v1.md section 3, G16: the snapshot is plaintext inside AF7's envelope) are the same
shape of remaining work.
