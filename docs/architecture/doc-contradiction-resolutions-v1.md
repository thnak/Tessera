# Document contradiction resolutions v1

Status: record of a documentation audit (2026-11-08). This file lists contradictions that were
found between documents, the rule used to resolve each one, and the items that are still
undecided. It does not define product behavior; the documents named in each row do.

## 1. Resolution rules

Applied in this order:

1. **R1**: [`AGENTS.md`](../../AGENTS.md) wins over every other document.
2. **R2**: an Accepted ADR in [`decisions/`](../../decisions/README.md) wins over specs, design
   documents and guides.
3. **R3**: a normative spec in `docs/specs/` wins over `docs/design/` and `docs/guides/`. Between
   documents of the same level, the document that owns the topic wins (for example
   [`runtime-config-v1.md`](../specs/runtime-config-v1.md) owns configuration keys and defaults).
4. **R4**: if no rule decides, the reading consistent with the Non-negotiables in `AGENTS.md` was
   chosen, or the point was marked "undecided" in every document that mentions it.

Terms: "V1" is defined in the [glossary](../reference/glossary.md). Big Steps A to F are defined in
[`AGENTS.md`](../../AGENTS.md).

## 2. Resolved

| # | Topic | Resolution | Rule | Files changed |
|---|---|---|---|---|
| 1 | Big Step of the pixel-golden pipeline | Big Step B2 (Big Step E extends the corpus) | R1 | `decisions/ADR-009-*`, `docs/design/renderer.md`, `docs/guides/testing.md`, `docs/README.md` |
| 2 | Big Step of frame pacing (`maxFps`, vsync, VRR) | Big Step B2; Big Step B3 keeps swapchain acquire/present | R1 | `docs/design/renderer.md` |
| 3 | Big Step of the WebGPU backend | Big Step D | R1 | `docs/design/renderer.md` |
| 4 | Pacer blocking the UI thread | The pacer never waits on GPU, swapchain, fence or I/O on the UI thread; between ticks the loop idles on the platform event/timer wait until the next deadline | R1 | `docs/design/game-loop.md` |
| 5 | Renderer batching key | `(scissor, texture)` with one pipeline, taken from the code | R4 (code) | `decisions/ADR-003-*` (one line), `docs/design/dirty-rendering.md`, glossary |
| 6 | Defaults of `backgroundMaxFps`, `resizeMode` | Values in `runtime-config-v1.md` (0 and `adaptive`) | R3 | `docs/design/performance.md`, `docs/design/window-resize.md` |
| 7 | Type of `WindowMetrics.logical_width/height` | `std::uint32_t`, as in `hal.md` | R3 | `docs/design/window-resize.md` |
| 8 | Order of stages in a tick | input, animation sampling, style, layout, paint, render | R3 | `docs/design/ui-model.md` |
| 9 | Input queue type | Bounded lock-free SPSC queue (ADR-017) | R2 | `docs/design/input.md` |
| 10 | sRGB/gamma policy | Not scheduled; no decision yet | R1 | `docs/design/antialiasing.md`, `renderer.md`, `resources.md` |
| 11 | Pause setting for hidden windows | Runtime config key `pauseWhenOccluded` | R3 | `docs/design/animation.md` |
| 12 | Out-of-memory handling | Fatal; goes to the root handler (device loss stays recoverable) | R1 | `docs/specs/error-model-v1.md` |
| 13 | Error type in memory-model | `std::expected<T, Error>`; `StatusOr` only as a fallback on toolchains without `std::expected` | R1 | `docs/specs/memory-model-v1.md` |
| 14 | License wording in dependencies | MIT (ADR-013) | R1/R2 | `docs/specs/dependencies.md` |
| 15 | Third-party notices file name | `THIRD_PARTY_NOTICES.md` | R1 | `docs/specs/dependencies.md` |
| 16 | Silent fallback of `gpuTimestamps`/`profilingBackend` | Falls back, reports one diagnostic, counts it in metrics | R1 | `docs/specs/diagnostics-config-v1.md` |
| 17 | Identifier format in devtools | JSON decimal strings | R3 | `docs/specs/devtools-protocol-v1.md` |
| 18 | Explicit `automationId` vs `UiId` | The explicit id replaces the computed `UiId`, so `uiId` equals it | R3 | `docs/specs/automation-ids-v1.md` |
| 19 | `Cancelled` in async data | Internal state; not a `DataStatus` value | R3 | `docs/specs/async-data-v1.md` |
| 20 | Devtools in release builds | Disabled by default in release | R2 (ADR-031) | `docs/specs/devtools-protocol-v1.md` |
| 21 | `catchEvents` default | `true` is decided; only per-build-type variation is open | R3 | `docs/specs/error-boundaries-v1.md` |
| 22 | `!important` order and unknown-property reports | UA > user > author with inline above author rules; two separate diagnostics with their own keys | R3 | `docs/specs/stylesheet-v1.md` |
| 23 | Scroll snap | Snap physics is a V1 feature; CSS `scroll-snap-*` properties are "Later" | R3 | `docs/specs/stylesheet-v1.md`, `scroll-and-gestures-v1.md` |
| 24 | `scroll-chain` | Not a CSS property; `allow_chain` is derived from `overscroll-behavior` | R3 | same two files |
| 25 | Gamepad scroll tests | Removed from the test list (not specified for V1) | R3 | `docs/specs/scroll-and-gestures-v1.md` |
| 26 | Text buffer structure and over-limit paste | Contiguous up to 64 KiB, then a piece table (ADR-033); over-limit paste is rejected whole | R2 | `docs/specs/text-editing-v1.md` |
| 27 | Shader manifest `tests` field | Required (at least one smoke test) | R3 | `docs/specs/shader-manifest-v1.md` |
| 28 | IR node kinds for virtualization | `VirtualList`/`VirtualGrid` as in `ir-v1.md` | R3 | `docs/specs/virtualization-v1.md` |
| 29 | i18n: Q-15 and unused keys | Q-15 decided; unused keys fail CI | R3/R4 | `docs/specs/i18n-v1.md` |
| 30 | Meaning of `navigation.modalStack: nested` | A modal-local stack, not per-tab stacks | R3 | `docs/specs/navigation-v1.md`, `project-config-v1.md` |
| 31 | Time source in audits | Simulation time for goldens/structural tests; only audit mode may use real time | R1 | `docs/specs/headless-automation-v1.md`, `lighthouse-audit-v1.md` |
| 32 | `Occluded` event | `WindowHidden` plus an occlusion flag in `WindowMetrics` | R3 | `docs/specs/os-events-v1.md` |
| 33 | Unknown deep-link route | The `notFound` route of `navigation-v1.md` | R3 | `docs/specs/deep-linking-v1.md`, `docs/guides/deep-linking.md` |
| 34 | G/Q ledger status | No gap is open: 25 resolved, 3 deferred (G-05, G-21, G-27) | R3 | `docs/architecture/app-completeness-cross-check-v1.md` |
| 35 | CLI `--targets` values | `windows`, `linux` | R3 | `docs/guides/project-generator.md` |
| 36 | Sample commands and template names | Only `blank` and `dashboard` are generator templates; other sample names are planned samples | R3 | `docs/guides/sample-apps.md` |
| 37 | Big Step of the shader spike | GLSL to SPIR-V from B1; Slang/Tint spike in Big Step D | R1 | `decisions/ADR-011-*` |

## 3. Undecided (a product or design decision is needed)

| # | Topic | Where it appears | Options |
|---|---|---|---|
| U1 | Default `box-sizing` | `docs/design/layout.md`, `docs/specs/stylesheet-v1.md` | `content-box` (CSS default) or `border-box` (the Yoga model) |
| U2 | Release of the bundled component library and the initial DataGrid | `docs/design/component-library.md`, `app-completeness-gap-resolutions-v1.md`, `docs/README.md`, ADR-032 | A release name, or "after V1" |
| U3 | Extra devtools clients | `docs/specs/devtools-protocol-v1.md` | read-only or rejected |
| U4 | How devtools is explicitly enabled in a release build | `docs/specs/devtools-protocol-v1.md` | Mechanism not specified |
| U5 | How shortcuts are marked as allowed during IME composition | `docs/specs/accessibility-v1.md`, `platform-shell-v1.md` | Mechanism not specified |
| U6 | Which Android versions use the glow instead of stretch | `docs/specs/animation-v1.md` | Not specified |
| U7 | `std::expected` and the language standard | `AGENTS.md` (C++20), `error-model-v1.md` | `std::expected` is a C++23 library feature; the project says C++20 with a `StatusOr` fallback |
| U8 | Sleeping between ticks vs non-negotiable 12 | `docs/design/game-loop.md` section 9 | The wording was accepted by this audit; confirm it matches the intended meaning of non-negotiable 12 |

## 4. References

- Terms: [glossary](../reference/glossary.md).
- Where each decision is owned: the documents named in the "Files changed" column.
