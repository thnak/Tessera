# Context: the owner decisions and the problem

Part of the [ADR-050 decision set](README.md). Section bodies here: `## Owner decisions (2026-11-12)` and `## Context`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

## Owner decisions (2026-11-12)

Recorded as given; the rest of this ADR is revised to follow them.

1. **Direction.** "Tessera IS an application framework, not just a UI toolkit with optional
   helpers: apps built on Flutter/React Native adopt the framework's programming model, so the
   framework should manage more (lifecycle, services, background work, state, storage,
   navigation). Keep the tiering and opt-in linkability only as an engineering/layering
   mechanism, and revise wording that frames it as 'optional helpers'; reconsider whether the app
   template should link the core appfw modules by default (recommend yes)."
2. **TSX access to services** (this correction supersedes an earlier answer that ruled out TSX
   hooks): "TSX DOES get service access, but only as `useService<T>()` where T must be an
   interface (abstract service contract), never a concrete type or string key; developers still
   implement/register the services in C++. Concrete implementations stay in C++ (developer class
   over gen::<Component>Base<Self>). [...] the hook is compile-time typed, lowered by the
   compiler/codegen to resolve-once-at-mount (typed pointer/ref cached in the component), no
   per-render lookup, no runtime string/type_index lookup; the compiler should reject a T that is
   not a registered interface and report missing providers at startup."
3. **Resolve cost.** "Needs careful study: the service RESOLVE mechanism may affect
   performance. Evaluate it with real numbers [...] Then conclude in the ADR which resolution
   mechanism is chosen and WHERE resolution may happen (mount only; never in the frame/hot
   path)." Measured in [`service-resolve-benchmark.md`](../../docs/design/service-resolve-benchmark.md);
   conclusion in section 3 and the appendix.
4. **(3b) Refs bound to native rendering.** "Patterns like native 3D drawing need a `ref`
   mechanism: a component gets a typed ref to a native render surface/host element (e.g. a
   viewport node that the app's C++ renders into through the RHI, with size/DPI/visibility/
   lifecycle callbacks, input forwarding, and damage/dirty integration with the frame pipeline,
   offscreen texture or direct sub-surface composited by the scene graph)." Section 7.

## Context

The audit ([`app-framework-gap-audit-v1.md`](../../docs/architecture/app-framework-gap-audit-v1.md))
finds that the frame loop, input, style, layout and async primitives are implemented, but the
structure that Qt, JUCE, Flutter and Jetpack give every app is missing or contradictory:

1. **No app object or app lifecycle.** `WindowHost::run()` ends on `Close`; `Focus`, `Blur`,
   `Occluded`, `Minimized` and `Restored` are dropped; there is no termination veto and no
   app-level foreground/background state. [`startup-api-v1.md`](../../docs/specs/startup-api-v1.md)
   rules out a lifecycle API in V1 while other specs need services to react to suspend/resume.
2. **No service model.** [`services/common-contract.md`](../../docs/design/services/common-contract.md#1-common-contract) defines only a synchronous
   `shutdown()`; nothing defines start order, failure, or bounded asynchronous stop.
3. **No way for components to reach app-scoped objects.** Context is rejected by
   [`component-model-v1.md`](../../docs/specs/component-model-v1.md) (its section 12 leaves
   "context-free dependency injection" open), yet `NavController`, `DataStore`, `ShellService`
   and `RestoreService` are specified as if components can call them.
4. **No app-wide state pattern** beyond component-local `useState` and local stores.
5. **Dependency direction is wrong on paper.** [`module-map.md`](../../docs/architecture/module-map.md)
   makes the runtime depend on services; services need the runtime's executors and events, so
   they must sit above it.
6. **No per-instance native render surface.** ADR-028 covers reusable custom elements declared in
   a manifest; an app that draws its own 3D view into one component has no typed handle to it.

The owner has set the direction (decision 1): Tessera is an application framework in the sense of
Flutter or React Native. Apps adopt its programming model, and the framework manages lifecycle,
services, background work, state, storage and navigation. The UI core stays separable as an
engineering property (it can be tested, embedded or ported alone), not as a product promise that
the app pieces are optional extras. The hard rules in [`CLAUDE.md`](../../CLAUDE.md) still bind: no
RTTI or hidden allocations on hot paths, never block the UI thread, no detached tasks, exceptions
only at boundaries, static linking, MIT.
