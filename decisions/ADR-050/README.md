# ADR-050: Application-framework layer above the UI runtime (lifecycle, services, task scopes, app state, native render surfaces)

- Status: Accepted (2026-11-12, owner; revised the same day with the owner decisions below)
- Date: 2026-11-12
- File name: this decision is a folder, `decisions/ADR-050/README.md` plus the linked parts;
  the former single file `ADR-050-opt-in-app-framework-layer.md` reached the 600-line hard
  limit. The slug is unchanged, so `ADR-050-opt-in-app-framework-layer` still names this ADR;
  the layer is no longer "opt-in" in the product sense (owner decision 1).
- Related: [ADR-012](../ADR-012-static-linking.md) (static linking),
  [ADR-013](../ADR-013-project-license-mit.md) (MIT),
  [ADR-021](../ADR-021-async-runtime.md) (async runtime),
  [ADR-023](../ADR-023-os-event-normalization-and-foldables.md) (OS events),
  [ADR-025](../ADR-025-error-boundaries.md) (error boundaries),
  [ADR-028](../ADR-028-custom-native-elements.md) (custom native elements, low-level rendering),
  [ADR-030](../ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n, data),
  [ADR-040](../ADR-040-generated-base-and-developer-class.md) (generated base + developer class),
  [ADR-042](../ADR-042-jsx-startup-surface.md) and [ADR-043](../ADR-043-launch-modes-and-state-reuse.md)
  (startup surface, launch modes), ADR-049 (backend extensibility, open as
  [PR #83](https://github.com/thnak/Tessera/pull/83)),
  [`app-framework-gap-audit-v1.md`](../../docs/architecture/app-framework-gap-audit-v1.md) (the audit
  this ADR answers), [`service-resolve-benchmark.md`](../../docs/design/service-resolve-benchmark.md)
  (measurements behind section 3), [`services/README.md`](../../docs/design/services/README.md),
  [`async.md`](../../docs/design/async.md).
- Terms: **core** = the UI runtime that exists today (`tessera_ui`, `tessera_runtime`,
  `tessera_app` host and window driver, rendering, HAL). **App framework** = the layer this ADR
  adds above the core. **Service** = a long-lived app-scoped object with a start/stop lifecycle
  (storage, navigation, data, ...). **Service interface** = an abstract C++ class that names a
  service contract; implementations are concrete C++ classes. **Composition root** = the code that
  creates the services and windows (the app's `main()` or the generated entry). **Task scope** = an
  owner of a group of tasks that can cancel them and knows when they have all finished.
  **Resolve** = turning a service id into a typed pointer.

This ADR is a folder because the decision reached the 600-line hard limit. The section bodies
live in the parts below; every heading keeps its original number, level and order, so a
reference to `section 3`, `section 3.1`, `section 5`, `section 7` or `section 8` still names the
same heading. A reference that used to address an anchor inside the single file now names the
part that holds the heading. The decision, its status and its history are unchanged.

## Parts

| Part | Owns |
|---|---|
| [`context.md`](context.md) | The four owner decisions of 2026-11-12 and the Context section (what the audit found missing or contradictory) |
| [`layers-and-lifecycle.md`](layers-and-lifecycle.md) | Sections 1 and 2: the layer table and dependency direction, `Application`, the app state table, the termination request, startup |
| [`service-provisioning.md`](service-provisioning.md) | Sections 3 and 3.1: `ServiceId`, the frozen registry, resolve once at mount, the option table and its measurements, `useService<T>()` in TSX |
| [`service-lifecycle.md`](service-lifecycle.md) | Section 4: the `Service` state machine, dependency order, start phases, bounded shutdown, error conversion, dispatch, and the AF1 implementation note |
| [`background-work-and-app-state.md`](background-work-and-app-state.md) | Sections 5 and 6: `TaskScope`, the layer-0 timer wake source, desktop/mobile policy, the headless entry, `Observable<T>`, stores, the rejected patterns (AF2, AF4) |
| [`native-render-surfaces.md`](native-render-surfaces.md) | Section 7: the ref model, the single `SurfaceRenderer` contract, ownership and lifetime, threading, dirty/static-idle rules, composition, the backend-agnostic seam (AF6) |
| [`modules-and-tiers.md`](modules-and-tiers.md) | Sections 8, 9 and 10: the module placement table, in-tree versus community tiers and contract suites, and the hard-constraint check |
| [`consequences-and-alternatives.md`](consequences-and-alternatives.md) | What the decision buys, the negative and risk list, and the alternatives that were rejected |
| [`roadmap-questions-and-follow-up.md`](roadmap-questions-and-follow-up.md) | The AF1-AF8 roadmap and its acceptance lines, the owner questions and the answers accepted on 2026-11-12, and the follow-up list |
| [`appendix-measurements.md`](appendix-measurements.md) | The measured resolve, use-site and dispatch numbers the section 3 decision rests on |
