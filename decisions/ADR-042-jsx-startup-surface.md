# ADR-042: A declarative JSX startup surface with a framework-provided loading screen

- Status: Accepted (2026-11-11; owner kept the member-tag carve-out and made the implicit App a permanent warning).
- Date: 2026-11-11
- Related: [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own JSX/TSX front end),
  [ADR-039](ADR-039-lowercase-html-tags-accepted.md) (precedent: a narrow, data-driven exception to
  a closed tag rule), [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) (normative contract),
  [`jsx-subset-v1-jsx.md`](../docs/specs/jsx-subset-v1-jsx.md) (member-tag rule amended),
  [ADR-014](ADR-014-project-config-format.md) (`tessera.json` and the C++ API),
  [ADR-024](ADR-024-native-first-boundaries.md) (no runtime JS).
- Amends: [`jsx-subset-v1-jsx.md`](../docs/specs/jsx-subset-v1-jsx.md) section 10.1 (member tags) as
  an explicit exception; [`jsx-subset-v1-jsx.md`](../docs/specs/jsx-subset-v1-jsx.md) section 11
  (startup elements are not intrinsics); [`docs/specs/project-config-v1.md`](../docs/specs/project-config-v1.md)
  `loading` (precedence note).

## Context

Every app has a cold-start problem, and the repository has the UX policy
([`startup-and-loading.md`](../docs/design/startup-and-loading.md)), the resource mechanics
([`resource-lifecycle-v1.md`](../docs/specs/resource-lifecycle-v1.md)) and the project config
(`tessera.json.loading`) — but no developer surface. A developer today would have to write phase
code, timers, preload calls and platform bootstrap by hand, which contradicts:

- **"Show something useful early"** and "no fake progress / no infinite spinner"
  ([`startup-and-loading.md`](../docs/design/startup-and-loading.md) section 1 and 4).
- **"Never block the UI thread"** ([ADR-021](ADR-021-async-runtime.md)): phase driving belongs to
  the runtime, not to app code.
- **Native-first boundaries** ([ADR-024](ADR-024-native-first-boundaries.md)): whatever the source
  surface is, it must lower to compile-time metadata and native C++, not to a runtime framework.

The V1 source subset blocks the obvious React-like spelling: member tags are rejected
(`TESSERA_JSX_SUBSET_DYNAMIC_TAG`), the intrinsic table is closed at 13 names, element-valued props
exist only for `ErrorBoundary.fallback`, object-literal props lower only as `style`, and component
references are not values. A design that keeps those rules cannot express "app root, replaceable
loading screen, prioritized resource hints" declaratively.

## Decision

1. **Startup is declared in TSX, as elements.** The app entry component returns `<App>` with
   declaration children (`<Startup.Loading>`, `<Startup.Resources>` with `<Resources.Font>` /
   `<Resources.Image>` / `<Resources.Data>`) and the initial UI tree. There is no `startup={{...}}`
   object prop and no `initialScreen` component reference: children express the initial UI, which
   avoids new value kinds in the IR.
2. **Member tags are accepted only for the built-in namespaces `Startup` and `Resources`.** They
   are imported by name from `"tessera"`, resolve in analysis against a closed member table, and
   never become runtime tags. Every other member tag keeps `TESSERA_JSX_SUBSET_DYNAMIC_TAG`. This
   is a data-driven exception in the spirit of ADR-039, not a general namespace feature.
3. **Declaration elements are hoisted.** They lower into a component-level `startup` record (resource
   hints, loading component id, `restoreState`) and produce no IR node and no `add_child` call. The
   App root itself is not a scene node.
4. **The framework owns a default loading screen that is always available and replaceable.**
   It is compiled into every app, needs no app resource (no text, no app fonts), is theme-token and
   vector driven, visually continuous with the platform launch artifact, accessible and
   reduced-motion aware. `<Startup.Loading>` replaces it; a failing replacement falls back to it.
5. **Phases are owned by a runtime `AppHost`.** The declaration is compiled into a static startup
   manifest; `AppHost` drives P0-P5, never blocks the UI thread, and gates the app UI only on the
   critical set (P4). Priorities `critical`/`high`/`low` map to the existing integer
   `RequestOptions.priority`.
6. **`tessera.json.loading` keeps policy, the declaration keeps structure.** Precedence is
   CLI > `AppConfig` > compiled declaration > `tessera.json` > defaults; direct overlaps
   (`criticalResources`, `screen`, `restoreState` vs `restore.enabled`) are union/override with a
   `TESSERA_STARTUP_CONFIG_SHADOWED` warning, never a silent second source of truth.

## Consequences

- The startup contract becomes compile-time checked: bad tags, duplicate declarations, missing
  assets, non-critical images in the loading subtree and reserved-name collisions are errors, not
  runtime surprises. This is the developer-experience win that motivates the decision.
- The JSX subset changes in a contained way: parser `check_tag_name` narrows for two namespaces,
  analysis resolves the dotted name against the built-in table, and the spec plus rejection fixtures
  and diagnostics goldens are amended. Namespace imports (`import * as`) stay rejected.
- The app generator gains artifacts it does not have today: a startup manifest, a generated entry
  point and an `AppHost`/`StartupPipeline` runtime. Until those land, this decision is
  docs-and-design only; the parser still rejects the new tags.
- The declaration cannot express anything the runtime cannot execute: until the resource manager
  exists, `critical`/`high`/`low` are validated and typed but not honored, and `restoreState` is a
  forwarded flag.
- The loading subtree gets a hard constraint (critical-only resources). This is checkable for
  images and normative for fonts, and it is what keeps "show a frame immediately" true.
- `App` is a library-level reserved name; a project cannot define a component or import named
  `App`, `Startup` or `Resources`.

## Alternatives

- **Props on a library `App`** (`resources={["font:app-font:critical"]}`, `loading="Loading"`,
  conditional children over a stage state). Smallest delta (a library-component row plus a builder,
  no parser or IR change), but the loading screen becomes a stringly-typed component name and the
  phases have to be driven by app code or a hook that does not exist. Rejected: it fails the
  "framework owns startup" goal and pushes phase logic back to the developer.
- **Flat built-in tags** (`<StartupLoading>`, `<ResourceFont>`). Same delta class as the props
  option with no parser reversal, but it pollutes the global tag namespace, reads worse, and does
  not scale to future groups (`Startup.Auth`, `Navigation.Route`). Rejected as the long-term
  surface; acceptable as a temporary shim only if the member-tag carve-out is rejected later.
- **Object-literal `startup={{...}}` prop and component-reference values** (the shape developers
  copy from React frameworks). Rejected: the IR has no record expression and no component-reference
  value; it is the largest delta and reverses two subset rules for no additional capability.
- **`tessera.json`-only startup config.** Rejected: no compile-time asset/linkage checks, no
  co-location with the code it configures, and it reopens the "strict schema, no duplicate source"
  problem.
- **Conditional children over a stage state** (`{stage === "loading" && <MyLoading />}`). Rejected in
  V1: the framework cannot drive a developer component's state, so the stage would be written by
  hand — the exact thing this decision removes.

## Follow-up

- Slices S2-S7 in [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) section 12, starting with the
  analyzer/lowering member-tag carve-out and the `startup` record, then codegen entry +
  `AppHost`, then the resource manager.
- When S2 lands, amend [`jsx-intrinsics-v1/machine-readable.md`](../docs/specs/jsx-intrinsics-v1/machine-readable.md)
  so editors complete the startup elements and their props.
- Revisit the props alternative if the parser carve-out proves too costly; the runtime contract
  (`AppHost`, manifest, phases) is independent of the source spelling and should not be redesigned
  if the spelling changes.
