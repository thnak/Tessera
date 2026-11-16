# Module placement, tiers, and hard constraints

Part of the [ADR-050 decision set](README.md). Section bodies here: `### 8.`, `### 9.` and `### 10.`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

### 8. Module placement (each module keeps or gets its own spec)

| Module | In-tree scope | Community scope |
|---|---|---|
| `storage` (template default) | settings key/value with versioned schema, atomic document writes, cache directory, migrations, keystore interface + Windows/Linux implementations | database adapters, cloud sync (SQLite placement is open question 4) |
| `navigation`, `restore`, `data`, `i18n` (template default); `shell` | as specified by [ADR-030](../ADR-030-app-platform-completeness.md) and its specs, now as modules | extra platform implementations |
| `permissions` (new spec) | state model (`NotDetermined`, `Granted`, `Denied`, `DeniedPermanently`) and just-in-time request flow | per-platform permission back ends beyond the in-tree platforms |
| `notifications` | local notifications, activation routing | push providers (FCM, APNs, Windows push) |
| `net` | HTTP client interface, cancellation/timeout/retry rules, deterministic fake | transport back ends (open question 5) |
| `updates` | the signed-manifest reference flow of [`updates-and-distribution-v1.md`](../../docs/specs/updates-and-distribution-v1.md) | store-specific and MDM updaters |

### 9. In-tree versus community

The tiers of ADR-049 apply unchanged to app-framework providers: Tier 1 in-tree and release
blocking (`appfw_core`, every module interface, its fake, and the Windows/Linux implementations);
Tier 2 community-maintained with a named maintainer and a passing contract suite; Tier 3
experimental, out of tree or behind an OFF option. Each module interface ships a **contract test
suite** run against its fake, which a provider links and runs. Contributed providers follow
ADR-049's licensing rules: MIT code, static linking, no GPL/LGPL, entries in
`docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`. The **contracts** (lifecycle states,
`ServiceContext`, `Service`, service interfaces, `TaskScope`, `Observable`, `SurfaceRenderer`,
module interfaces) stay core-owned and change only by ADR or spec.

### 10. Hard-constraint check

| Constraint | How this design meets it |
|---|---|
| No RTTI or reflection | compile-time ids plus per-type address tags; requirements are C++ types checked by `static_assert`; TSX `useService` types are checked against a manifest at compile time |
| No hidden allocations on hot paths | fixed-capacity registries frozen at start; resolution only at mount (0 allocations measured); subscriptions allocate at mount only; surface targets created on attach/resize |
| Never block the UI thread | async `start`/`stop`; shutdown and termination are tick-driven; surface texture handoff checks completion without waiting |
| Static UI idles | timers and continuous surfaces join the pacer deadline; on-demand surfaces redraw only on request |
| No detached tasks | tasks start only inside a `TaskScope`; scopes close under a deadline |
| Exceptions only at boundaries | the service and renderer boundaries catch and convert to `Failed` plus a diagnostic |
| Static linking, MIT | every layer is a static library; providers follow ADR-049 section 5 |
