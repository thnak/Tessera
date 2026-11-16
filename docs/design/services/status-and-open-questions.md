# Status and open questions

Part of the [services design set](README.md). Section 9 is the implementation order and section 10
the list still open.

## 9. Status

Services are post-V1 (V1 = first-release scope, see [AGENTS.md rule 8](../../../AGENTS.md#non-negotiables); the work is in [Big Step F](../../../AGENTS.md#big-steps-group-work-dont-micro-step)). The interfaces in this folder are targets; implementation order:
storage/settings, navigation/restore, i18n, platform shell, networking, async data,
notifications.

## 10. Open questions

- Which HTTP/TLS stack (platform vs vendored) and static-link implications.
- Localization catalog format and build pipeline.
- Navigation integration with the compiler/IR.
- Background execution policy on mobile.
- Secret storage abstraction across platforms.
- ~~Who marks the in-frame service-lookup region.~~ **Decided by AF3 (issue #175); see
[`app-lifecycle-rulings.md`](app-lifecycle-rulings.md) section 11.**
  ADR-050 section 3 item 5 forbids resolving a service while a frame is being updated and defines a
  counter for it, but the region has to be marked by somebody. AF1 (issue #173) ships the marker as
  `appfw::FrameScope` and the counters as `service_lookup_audit()`, and deliberately does **not**
  wire it into `runtime::RuntimeLoop`: layer 0 gains seams, not concepts (ADR-050 section 1 rule 1),
  and a whole-tick mark would count a service's own legal `start()` lookup, because a start resumes
  from the completion drain that runs inside the tick.
