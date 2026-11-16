# Consequences and alternatives

Part of the [ADR-050 decision set](README.md). Section bodies here: `## Consequences` and `## Alternatives considered`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

## Consequences

Positive:

- Apps get one programming model, as on Flutter or React Native: lifecycle, services, background
  work, state, storage and navigation are managed by the framework and present in every new app.
- One answer to "how does a component reach X": C++ `Requires` or TSX `useService<Interface>()`,
  checked at compile time and validated against providers at startup, with a use-site cost equal
  to constructor injection (measured).
- Native 3D and custom drawing get a typed, lifecycle-safe handle that respects dirty rendering,
  static idle and backend independence.
- Lifecycle, background and shutdown behavior are testable headlessly: scripted events,
  `ManualClock`, `ManualExecutor`.
- The layer split still lets layer 0 be tested, embedded or ported alone.

Negative and risks:

- A large layer to maintain, and [`startup-api-v1.md`](../../docs/specs/startup-api-v1.md),
  [`services/`](../../docs/design/services/README.md), module-map and component-model texts must be revised.
- Default-linked modules raise the floor of every app's binary and build time; mitigated by
  section garbage collection and per-app opt-out in `tessera.json`.
- Missing providers are found at startup, not at compile time: the composition root is ordinary C++
  and cannot be checked against every component's needs statically without making the app type a
  template parameter everywhere.
- Two manifests (`services.json`, `elements.json`) and generated `.d.ts` files to keep in sync with
  C++ headers; drift is a build error, not a silent mismatch.
- The opaque `app_context` pointer is untyped inside layer 0; misuse is prevented only by the
  generated base being the sole reader.
- Service provisioning, `useService` and ref codegen depend on ADR-040's `Base<Self>` codegen,
  which is not implemented yet.
- `virtual` service interfaces depart from ADR-040's CRTP-only style; the cost is measured as
  negligible for per-action calls but blocks inlining, so per-frame calls must use `final` types.

## Alternatives considered

- **Stay a UI toolkit; leave app structure to each app.** Rejected by owner decision 1.
- **Put services in the core runtime** (what the current module map implies). Rejected: inverts
  the layering and makes layer 0 untestable without the app pieces.
- **Qt-style meta-object system** (signals, properties, runtime lookup by name). Rejected: needs
  reflection or a moc-like generator for every C++ type, which the RTTI rule excludes.
- **Constructor injection through generated parents** (A), **lookup per use** (B'),
  **singletons** (C), **React context** (D), **runtime locators** (E) and **a DI library** (F):
  see section 3 and the measurements.
- **`useService` with concrete types or string keys.** Rejected by owner decision 2: TSX names
  contracts only, implementations stay in C++.
- **`useStore` / Redux-style store as the only state model** and **signals with automatic
  tracking**: see section 6.
- **Native surface through a raw platform child window** (HWND/NSView). Rejected for V1: breaks
  clipping, transforms and z-order inside the scene, and is not portable to Wasm.
