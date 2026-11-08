# Module map

Status: reflects the repository at [Big Step B1](../../AGENTS.md#big-steps-group-work-dont-micro-step)
(renderer core) plus the implemented B2 runtime slices: stylesheet engine, Yoga layout,
text-measurement stub, normalized input, fixed-step loop, dirty rendering/damage, diagnostics
counters, async primitives, and the animation runtime. Remaining B2/B3 work and empty directories
are marked **planned**; a planned directory is a seam, not an implemented feature. In the Status columns, B1, B2, and B3 are the phases of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step) (renderer core, runtime slice, window/swapchain/OS); C, D, and F are Big Steps C (compiler vertical slice), D (design-time preview), and F (production breadth); a letter in parentheses, such as "(D)", is the Big Step in which the directory is planned.

## 1. Top level

| Path | Responsibility | Status |
|---|---|---|
| `include/tessera/` | Public headers; light, no backend/OS headers | B1 subset |
| `src/core/` | object, memory, threading, events, resources | planned |
| `src/hal/` | window, display, input, lifecycle, power, timer, filesystem, graphics, audio | timer B2; rest planned |
| `src/hal/timer/` | monotonic clock + bounded idle wait (`tessera_hal_timer`) | B2 |
| `src/platform/` | Windows/X11/Wayland/Cocoa/Android implementations of HAL (hardware/OS abstraction layer) | planned (B3) |
| `src/ui/` | view, scene, layout, style, animation, responsive, text, editing, overlay, forms, accessibility | scene B1; style/layout/text/input/animation B2 core; view/responsive/accessibility planned |
| `src/ui/input/` | normalized input event, bounded queue, move coalescing | B2 core |
| `src/ui/style/` | CSS tokenizer, parser, selectors, cascade, media queries, `@supports`, keyframes, property registry | B2 core |
| `src/rendering/rhi/` | thin RHI (render hardware interface) interfaces | B1 |
| `src/rendering/gpu/` | batched 2D renderer | B1 |
| `src/element/` | custom native element runtime, registry, snapshots | planned (B2/C) |
| `src/rendering/null/` | deterministic no-GPU backend | B1 |
| `src/rendering/backend/vulkan/` | Vulkan 1.3 backend (volk + VMA (Vulkan Memory Allocator)) | B1 offscreen |
| `src/rendering/backend/webgpu/` | WebGPU/Dawn backend | planned (D) |
| `src/rendering/software/` | deterministic CPU/software path | planned (B2/B3) |
| `src/rendering/resources/` | textures, samplers, uploads, caches | planned |
| `src/rendering/scheduler/` | frame scheduling, fences, presentation | planned (B3) |
| `src/compiler/analyzer/` | own JSX/TSX front end + React-like rules | planned (C) |
| `src/compiler/rules/` | framework/binding rules | planned (C) |
| `src/compiler/ir/` | neutral IR (intermediate representation) + serialization | planned (C) |
| `src/compiler/codegen/` | IR -> C++ | planned (C) |
| `src/compiler/merger/` | protected-region merge | B1 |
| `src/runtime/` | fixed-step accumulator, clock, `RuntimeLoop`, frame pacer, async primitives, diagnostics counters | B2 core |
| `src/services/` | i18n (internationalization), navigation, state restore, storage, networking, notifications, shell | planned (F) |
| `src/data/` | async resource cache, mutations, offline queue | planned (F) |
| `src/diagnostics/` | logging, tracing, profiling, crash reporting | planned (B2); counters live in `src/runtime/` |
| `src/devtools/` | local inspector/hot-reload protocol | planned (C/D) |
| `src/updates/` | signed manifest, staging, rollback | planned (F) |
| `designer/web/` | Wasm (WebAssembly) + WebGPU viewer, live view, IPC (inter-process communication) | planned (D) |
| `designer/native/` | native host/IPC | planned (D) |
| `tests/unit/` | UI/compiler unit tests (CTest) | B1 subset |
| `tests/gpu/` | GPU/RHI contract/resource/frame tests (CTest) | B1 subset |
| `tests/integration/` | cross-module tests (GPU under `integration/gpu/`) | planned |
| `tests/golden/` | deterministic [goldens](../reference/glossary.md) + report (GPU under `gpu/`) | planned |
| `tests/fixtures/` | fonts, images, merger/compiler/GPU fixtures | B1 merger |
| `tests/benchmarks/` | performance budgets (GPU under `gpu/`) | planned |
| `samples/hello`, `samples/dashboard` | sample projects | placeholder |
| `tools/poc/` | Phase 0/B1 proofs | B1 |
| `tools/shaders/`, `tools/font/` | shader/font tooling | planned |
| `third_party/` | vendored dependencies (stb, volk, vma) | B1 |
| `decisions/` | ADRs | 37 ([ADR-001](../../decisions/ADR-001-vulkan-1-3-baseline.md) (Vulkan 1.3 baseline) through [ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) (self-contained WebAssembly designer)) |
| `docs/` | specs, design, guides, architecture | this set |

## 2. Dependency direction

```text
core
 ├─ hal ── platform/{windows,linux,macos,android}
 ├─ ui ── style, layout, animation, text, responsive, accessibility
 ├─ rendering ── rhi ── backend/{vulkan,webgpu,null,software}
 ├─ element ── rhi + shader manifest (custom native elements)
 ├─ data ── services (cache/offline)
 ├─ services
 ├─ diagnostics
 └─ runtime ── ui + rendering + element + hal + services + data + diagnostics

compiler (build time only): analyzer -> rules -> ir -> codegen -> merger
designer (Wasm): runtime/ui/rendering + webgpu + IPC
```

Rules:

- `core` has no OS/GPU/UI dependencies.
- `hal` has no UI/renderer dependencies; platform code implements HAL interfaces.
- `ui` may use RHI *types* (`Rect`, `Mat3`, `Color`) but never backend headers.
- `element` may use the public RHI/shader API; it never includes backend/OS headers.
- `rendering` consumes immutable display lists; it does not mutate the [scene graph](../reference/glossary.md).
- `runtime` orchestrates; it does not contain compiler/parser code.
- `compiler` is build-time only and must not be linked into production.
- `diagnostics` is cross-cutting but must not pull UI/renderer internals into `core`.

## 3. Public headers

Only `include/tessera/` is public. Current headers:

| Header | Purpose |
|---|---|
| `rendering/rhi/types.hpp` | geometry, color, descriptors, enums, `Mat3` |
| `rendering/rhi/device.hpp` | `Device`, `Buffer`, `Texture`, `Pipeline`, `RenderTarget`, `CommandList` |
| `rendering/gpu/vertex2d.hpp` | `Vertex2D`, attributes, push constants |
| `rendering/gpu/batched2d_renderer.hpp` | batched 2D renderer |
| `element/custom_element.hpp` | custom native element interface/contexts (planned) |
| `rendering/damage.hpp` | damage rectangles: clip, merge, cap, full-redraw fallback |
| `rendering/redraw_plan.hpp` | damage rects -> scissor plan + full-redraw policy |
| `rendering/null/null_device.hpp` | null device + command stats |
| `rendering/vulkan/vulkan_device.hpp` | Vulkan device factory (backend-specific) |
| `rendering/vulkan/batched2d_pipeline.hpp` | Vulkan batched2D pipeline factory |
| `hal/timer/steady_time.hpp` | monotonic seconds + bounded idle wait (B2) |
| `ui/scene/node.hpp` | retained scene node, hit testing, dispatch, dirty flags |
| `ui/scene/dirty_pass.hpp` | pruned dirty collect/clear over a scene subtree |
| `ui/scene/event.hpp` | pointer/keyboard/focus event types, phases, key payload |
| `ui/scene/focus.hpp` | focus target, focus transitions, depth-first Tab traversal |
| `ui/scene/input_dispatch.hpp` | pointer/keyboard input -> scene dispatch, focus + capture router |
| `ui/dirty.hpp` | style/layout/paint/render/resource/GPU dirty bitmask |
| `ui/input.hpp` | normalized input event, bounded SPSC queue, sampler |
| `ui/style/token.hpp` | CSS token types and source slices |
| `ui/style/tokenizer.hpp` | streaming CSS tokenizer with source offsets |
| `ui/style/parser.hpp` | tolerant rule/declaration/at-rule AST |
| `ui/style/selector.hpp` | selector AST, combinators, attributes, specificity |
| `ui/style/selector_match.hpp` | StyleNode view, right-to-left matcher, selector index |
| `ui/style/cascade.hpp` | cascade ordering, computed style, inheritance/keywords |
| `ui/style/compiled_stylesheet.hpp` | parsed rules -> index -> cascade -> computed style |
| `ui/style/property_registry.hpp` | supported properties, initial values, shorthand expansion |
| `ui/style/media_query.hpp` | media-query parsing/evaluation against fixed context |
| `ui/style/keyframes.hpp` | `@keyframes` block parsing into offset steps |
| `ui/style/supports.hpp` | `@supports` condition parser/evaluator against the registry |
| `ui/style/color.hpp` | CSS color parser/formatter + sRGB interpolation |
| `ui/text/font_face.hpp` | FreeType-backed font file loading + design-unit metrics |
| `ui/text/font_metrics.hpp` | ascent/descent/line-gap provider + deterministic stub |
| `ui/text/freetype_metrics.hpp` | real ascent/descent/line-gap scaled from a loaded face |
| `ui/text/text_shaper.hpp` | shaped glyph/cluster/advance seam |
| `ui/text/harfbuzz_shaper.hpp` | HarfBuzz UTF-8 shaping with pixel advances |
| `ui/text/glyph_raster.hpp` | FreeType 8-bit glyph coverage rasterization (B2) |
| `ui/text/glyph_atlas.hpp` | bounded deterministic RGBA8 glyph atlas + page LRU (B2) |
| `ui/text/text_renderer.hpp` | shaped run -> atlas quads through Batched2DRenderer (B2) |
| `ui/text/text_measure.hpp` | text measurement seam + deterministic stub metrics |
| `ui/text/text_layout.hpp` | wrapped line boxes (width/height/baseline) |
| `ui/text/text_measure_cache.hpp` | bounded LRU measurement cache + stats |
| `ui/animation/easing.hpp` | linear/cubic-bezier/steps easing parser + evaluator |
| `ui/animation/animation_shorthand.hpp` | CSS `animation` shorthand and longhand list parser |
| `ui/animation/animation_start.hpp` | keyframe lookup + parsed animation spec -> runtime entries |
| `ui/animation/animation_style.hpp` | computed-style animation starts, idempotent ensure, removal sync, play-state updates |
| `ui/animation/loop_bridge.hpp` | tick animations + request RuntimeLoop frames for active/finished animations |
| `ui/animation/keyframes.hpp` | fixed-step `@keyframes` sampling + direction/fill |
| `ui/animation/runtime.hpp` | active/settled entries, tick, callback-safe completion, post-callback sampling, target/name state/pause/resume/stop/restart, id restart, target/property stop, reduced-motion clamping, dirty reporting |
| `ui/animation/spring.hpp` | semi-implicit Euler spring with retarget/epsilon settle |
| `ui/animation/spring_runtime.hpp` | active spring entries -> property declarations, reduced-motion snap |
| `ui/animation/property_dirty.hpp` | property -> layout/paint/render dirty-domain mapping |
| `ui/animation/style_overlay.hpp` | animation samples -> per-target `ComputedStyle` overlay |
| `ui/animation/transition.hpp` | CSS `transition` shorthand + longhand expansion parsers |
| `ui/animation/transition_runtime.hpp` | transition matching, string/numeric starts, computed-style diffs, interruption |
| `ui/layout/layout_engine.hpp` | computed-style to Yoga mapping + boxes + cached measure callback |
| `ui/layout/apply_layout.hpp` | absolute boxes -> parent-relative scene bounds + dirty propagation |
| `ui/layout/pixel_snap.hpp` | paint-time edge snapping (`round_half_up`, DPI scale clamp) |
| `ui/layout/damage_pass.hpp` | dirty bounds -> damage rects + previous-bounds tracker |
| `ui/frame_pipeline.hpp` | one-frame orchestration: animation tick -> target dirty domains -> damage -> normalized redraw plan |
| `runtime/async.hpp` | cancellation + callbacks + executor + completion queue (B2) |
| `runtime/diagnostics.hpp` | per-frame/total frame diagnostics counters |
| `diagnostics/log.hpp` | structured levels/categories, redaction, bounded ring sink (B2) |
| `diagnostics/metrics.hpp` | fixed-capacity atomic counters/gauges + snapshots (B2) |
| `runtime/fixed_step.hpp` | deterministic fixed-step accumulator (B2) |
| `runtime/clock.hpp` | clock interface + deterministic manual clock (B2) |
| `runtime/system_clock.hpp` | `Clock` adapter backed by the HAL timer (B2) |
| `runtime/runtime_loop.hpp` | fixed-step driver + render wake + completion drain (B2) |
| `runtime/frame_pacer.hpp` | deadline-based `maxFps` pacer, no sleep/busy-wait (B2) |

`src/compiler/merger/include/tessera/compiler/merger.hpp` is a build-time public header for the
merger prototype.

## 4. Build targets (current)

| Target | Type | Purpose |
|---|---|---|
| `tessera_rhi` | static | RHI interfaces |
| `tessera_gpu` | static | batched 2D renderer |
| `tessera_rhi_null` | static | [null backend](../reference/glossary.md) |
| `tessera_scene` | static | scene graph/events, focus manager, keyboard routing |
| `tessera_hal_timer` | static | monotonic clock + bounded idle wait (B2) |
| `tessera_diagnostics` | static | structured logging + metrics core (B2) |
| `tessera_runtime` | static | fixed-step/clock/pacing core + system clock (B2) |
| `tessera_input` | static | normalized input queue + sampler (B2) |
| `tessera_damage` | static | damage rectangle accumulator (B2) |
| `tessera_style` | static | CSS tokenizer (B2) |
| `tessera_yoga` | static | vendored Yoga flexbox engine |
| `tessera_freetype` | interface | FreeType (FetchContent, pinned archive) for font loading |
| `tessera_harfbuzz` | interface | HarfBuzz (FetchContent, pinned archive) for shaping |
| `tessera_text` | static | font loading/metrics + shaping + measurement seam + deterministic stub + LRU cache (B2) |
| `tessera_text_render` | static | glyph atlas + shaped text batching on `tessera_gpu` (B2) |
| `tessera_animation` | static | easing + keyframes + runtime + springs (B2) |
| `tessera_layout` | static | computed-style to Yoga layout adapter + text measure callback (B2) |
| `tessera_frame` | static | per-frame animation/dirty/damage/redraw orchestration (B2) |
| `tessera_merger` | static | protected-region merger |
| `tessera_vulkan` | static | offscreen Vulkan renderer |
| `tessera_rhi_vulkan` | static | Vulkan RHI backend + batched2D pipeline |
| `tessera_stb` | static | stb image write |
| `vulkan_offscreen_poc` | exe | offscreen Vulkan proof |
| `batched2d_poc` | exe | batched 2D demo (Vulkan/null) |
| `merger_poc` | exe | merger CLI (command-line interface) |
| `*_tests` | exe | unit/CLI tests |

## 5. Where to add code

- New UI widget: `src/ui/view/` + `include/tessera/ui/view/`; no backend headers.
- New platform window backend: `src/platform/<os>/` implementing `src/hal/window/`.
- New HAL seam: `src/hal/<area>/` + `include/tessera/hal/<area>/`; the HAL never includes
  `src/runtime/`.
- New GPU backend: `src/rendering/backend/<api>/`; add a null fallback/test first.
- New compiler pass: `src/compiler/{analyzer,rules,ir,codegen}/`; IR is the only codegen input.
- New service: `src/services/<name>/`; async, cancellable, no UI blocking.
- New diagnostic sink: `src/diagnostics/<area>/`; bounded and redacted.
- New test: UI/compiler in `tests/unit/<area>_tests.cpp`; GPU in `tests/gpu/<area>/` +
  `tests/gpu/CMakeLists.txt`.
- New doc: `docs/{architecture,design,specs,guides,reference}/`; update [`docs/README.md`](../README.md).
- New decision: `decisions/ADR-XXX-slug.md` + [`decisions/README.md`](../../decisions/README.md).

## 6. Ownership rules

- The compiler owns everything outside [protected regions](../reference/glossary.md); developers own inside them.
- Generated files are never edited outside `[DEVELOPER_LOGIC_START/END]`.
- The [binding manifest](../reference/glossary.md) is the source of truth for binding ids.
- UI code never owns a window/swapchain; the HAL/runtime does.
- GPU resources are owned by the device; destroy resources before the device.
- Specs are normative; design notes explain; ADRs record contentious choices.

## 7. Naming and size

- Namespaces `tessera::<area>::<module>`.
- One primary type per header; soft 300 / hard 500 lines ([ADR-015](../../decisions/ADR-015-code-style-build-cache.md) (code style and build cache)).
- Public headers stay light; heavy third-party headers in `.cpp`/`detail/`.
- Tests are deterministic and do not read the wall clock, network, or system fonts.
