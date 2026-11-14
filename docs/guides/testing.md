# Testing guide

Status: the Big Step B1 (core renderer) and B2 runtime-slice unit tests run with CTest. A
lavapipe golden harness now covers the batched2d proof (`batched2d_golden`, Linux with Mesa
lavapipe); the broader golden corpus and CPU-rasterizer path remain B2 work (see
[AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step); Mesa lavapipe is a
software Vulkan driver, see
[ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md), deterministic headless
golden tests). The target test pyramid is in [design/testing.md](../design/testing.md).

## 1. Run the tests

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

Linux CI uses lavapipe (software Vulkan ICD, installable client driver):

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --preset ci
```

## 2. Current tests

| Test | What it covers |
|---|---|
| `merger_unit` | protected-region extraction/merge/idempotency/CRLF/warnings |
| `rhi_null` | null backend (RHI backend with no GPU) device commands, buffer round-trip |
| `scene_graph` | hit testing, capture/target/bubble, stop propagation |
| `scene_dirty` | dirty domains, ancestor aggregation, clear/recompute, setter invalidation |
| `dirty_pass` | pruned collect by domain, parent-first order, clear, node cap |
| `fixed_step` | fixed-timestep accumulator, clamp, catch-up cap, time scale |
| `clock` | deterministic manual clock, monotonic advance validation |
| `runtime_loop` | clock-driven steps, wake requests, continuous/on-demand render modes |
| `runtime_loop_async` | completion budget, pending wake, callback frame/dirty requests, no-queue path |
| `diagnostics` | frame sampling, totals, full-redraw count, reset, default state |
| `frame_pacer` | uncapped/deadline waits, lateness, cadence reset, refresh-divisor and VRR intervals, scripted rate changes (cap/refresh/VRR), validation |
| `async` | cancellation propagation/callbacks, FIFO/nested posting, bounded overflow, skip cancelled, budget |
| `completion_queue` | visible/layout/background priority, FIFO, per-tick budget, cancellation, overflow |
| `input` | bounded queue FIFO/overflow, modifier flags, adjacent-move coalescing |
| `input_dispatch` | pointer mapping, hit-test target, capture/target/bubble, outside/unsupported |
| `input_router` | pointer capture, move/up outside, release on up/cancel, reset, key retention |
| `damage` | clip, overlap/touch merge, margin, cap/full-redraw fallback |
| `redraw_plan` | integer edge rounding, clipping, full-redraw plan, area/scissor-count policy |
| `tokenizer` | CSS tokens, offsets, escapes, strings, numbers, functions, match tokens, ranges |
| `parser` | rule/declaration/at-rule AST, `!important`, custom properties, tolerant recovery |
| `selector` | combinators, attributes, pseudos/functions, robustness, specificity |
| `selector_match` | right-to-left matching, combinators, attributes, state/structural pseudos, index |
| `cascade` | origin/importance/specificity/source order, property normalization, inheritance keywords |
| `compiled_stylesheet` | compile rules, selector list specificity, cascade composition, inheritance, recovery |
| `property_registry` | metadata, box/pair/flex expansion, diagnostics, initial values |
| `media_query` | width/height/resolution, orientation, color scheme, reduced motion, lists/not |
| `keyframes` | from/to/percent offsets, comma selectors, invalid steps |
| `supports` | property/value support, custom properties, not/and/or precedence and nesting |
| `color` | named/hex/rgb/rgba/hsl parsing, format, sRGB interpolation, invalid input |
| `layout` | flex row/column, grow, gap/padding, absolute, percent/min/max, ratio, RTL, wrap, text measure, determinism |
| `layout_cache` | repeated layout hits text cache, same boxes, capacity injection, changed text refetch |
| `text_measure` | stub advance, word wrap, long-word break, newlines, letter spacing, style parsing |
| `text_layout` | wrapped/nowrap lines, long-word breaks, newline paragraphs, spaces, empty |
| `font_metrics` | stub ascent/descent/gap, default/custom baseline + line height, explicit override |
| `text_measure_cache` | key dimensions, LRU eviction, hit/miss stats, invalidate/clear, capacity guards |
| `layout_apply` | id matching, parent-relative bounds, dirty propagation, missing/extra boxes |
| `pixel_snap` | half-up/negative symmetry, scales 1/1.25/2, clamp below 1, joined edges |
| `damage_pass` | Paint/Render filtering, snapped world bounds, merge, DPI scale, cap/full-redraw, overflow |
| `damage_tracker` | old+new bounds, unchanged dirty, prune removed, retention eviction, capacity guard |
| `frame_pipeline` | animation targets -> dirty domains, needs_style/needs_layout, damage/scissor plan, full-redraw normalization, idle skip, invalid config |
| `animation` | linear/unit/discrete interpolation, delay/fill, reverse/alternate, infinite iterations |
| `animation_shorthand` | keyword ordering, comma lists, defaults, two times, directions/fills/play states, invalid specs |
| `animation_longhand` | eight independent lists, cycling, defaults, ms/s and negative delays, infinite/zero, all-or-nothing rejection |
| `animation_start` | timing mapping, delay/fill, reverse final, zero duration, infinite, callback, skip guards |
| `animation_lookup` | exact/case-sensitive name lookup, last duplicate wins, batch starts including paused, missing blocks skipped |
| `animation_pipeline` | parse -> lookup -> start -> tick -> overlay, dirty/wake, multiple animations, fill-forwards final |
| `animation_style` | computed-style longhands, shorthand fallback, longhand precedence, paused starts held, invalid/missing starts nothing, overlay |
| `animation_play_state` | style play-state pause/resume of active names, idempotence, longhand precedence, shorthand fallback, invalid no-op, fills untouched |
| `animation_ensure` | repeated style starts reuse ids, no duplicates, pause update, settled fill skipped, shorthand, invalid/missing no-op |
| `animation_sync` | absent names stop active/fill entries, none clears all, add/remove lifecycle, invalid no-op, case/target isolation |
| `animation_loop` | active ticks request frames, finish requests one final frame, paused/fill/idle do not wake, invalid dt wakes active |
| `animation_runtime` | start/tick/finish, callbacks, stop/clear, target/property stop, wake state, dirty domains |
| `animation_fill` | retained forward/both finals, no wake or repeat callback, overlay persistence, supersession, stop/clear cleanup |
| `animation_callback` | completion callbacks see retained fills, clear/stop removes them, same name/property starts supersede, order preserved |
| `animation_tick_reentrancy` | callback clear drops samples/dirty, sibling stop drops its sample, fill restart returns zero sample, callback start samples at zero |
| `animation_reduced_motion` | opacity clamps to 120 ms, mixed/non-opacity instant, toggle restores full timing, springs snap and retarget immediately |
| `animation_pause` | per-id/target pause/resume, held samples, no completion or wake while paused, invalid dt wake rules |
| `animation_pause_name` | exact target/name pause/resume counts, idempotence, sibling/target isolation, paused-only sleep, fills ignored |
| `animation_state` | const active/settled counts by target/name, paused included, empty/case/unknown zero, cleanup consistency |
| `animation_stop` | target-wide stop count, paused entries removed, other targets preserved, no callbacks, target zero |
| `animation_stop_name` | exact target/name stop, active and retained fills, case sensitivity, siblings preserved, no stopped callback |
| `animation_restart` | active elapsed reset/unpause, retained fill revival, callback repeat, exact name/target isolation |
| `animation_restart_id` | id restart resets active/paused, revives fills with id/property/callback, rejects zero/unknown, others untouched |
| `easing` | linear/presets/bezier identity/steps variants, parser rejection, sampler integration |
| `spring` | exact semi-implicit step, convergence/epsilon stop, retarget, invalid config, determinism |
| `spring_runtime` | sample declarations, layout/paint dirty, settle/finish, retarget final, callbacks, dt guards |
| `animation_overlay` | target filtering, later-sample precedence, property merge, unanimated preservation |
| `transition` | shorthand lists, time units, bezier/steps commas, defaults, invalid specs |
| `transition_longhand` | property/duration/delay/easing lists, cycling, defaults, negative delay, invalid slots |
| `transition_runtime` | exact-vs-all match, 0→1 sample, fill-final, zero duration, style diff, units/colors, invalid guards |
| `transition_lifecycle` | supersede same target/property, preserve other keys, current-value restart, callback suppression |
| `transition_pipeline` | longhand parse → style diff → tick → overlay, dirty/wake, untouched properties, settled style |
| `batched2d` | batch/vertex counts on the null backend |
| `batched2d_vulkan_smoke` | Vulkan 1.3 smoke render |
| `batched2d_vulkan_validation` | Vulkan render with validation layers (local GPU or lavapipe) |
| `batched2d_golden` | lavapipe bit-exact batched2d dashboard baseline (Linux only) |
| `batched2d_overlay_golden` | lavapipe overlay baseline, 1 LSB + 0.1% budget (Linux only) |
| `batched2d_text_golden` | lavapipe text-stack baseline, 1 LSB + 0.1% budget (Linux only) |
| `batched2d_shapes_golden` | lavapipe rounded/texture/clip/transform baseline, bit-exact (Linux only) |
| `safe_area` | SafeArea inset arithmetic (`include/tessera/hal/safe_area.hpp`): edges, sources, minimum, start/end under RTL, cutout and hinge insets, hostile values, event coalescing |
| `app_safe_area` | SafeArea in the app host: padding or margin from the metrics before layout, per-edge keyboard easing under the manual clock, reduced motion, no layout for unchanged insets |
| `library_safe_area_lowering`, `library_safe_area_runtime` | the library `SafeArea` lowers, validates and generates C++ (goldens `tests/golden/codegen/SafeArea*`); the generated page runs in the host on a notched phone, a gesture phone, a keyboard tablet and a book-mode foldable |
| `gallery_golden_*` | 23 lavapipe goldens of the library gallery under `tests/golden/gallery/` (the Button page: 3 themes x none/hover/active/focus, plus compact, 150% text and RTL; the SafeArea page: 4 devices x light/dark), 1 LSB + 0.1% budget; skip unless lavapipe is selected; CI uploads frame/diff/report as `gallery-golden-artifacts` (`gallery --golden FILE --update-goldens` rewrites) |
| `gallery_capture` | one gallery capture on any Vulkan device (smoke; skips without one) |
| `gallery --inspect` | not a CTest entry: `gallery --inspect DIR` renders the 23-case gallery matrix on any Vulkan device and writes one PNG per case plus a contact sheet. For eye review only — the images come from the installed rasterizer and are not comparable with the lavapipe goldens. This is how a developer with a hardware GPU sees the app; see [shell design preview](../design/shell-design-preview.md#3-regenerating) |
| `harness_frame_capture` | software-device capability rule, offscreen harness capture vs the committed `tests/golden/baseline/harness/` baseline (1 LSB + 0.1% budget, lavapipe only; gpu-smoke self-comparison elsewhere; `--update-goldens` regenerates) |
| `batched2d_windowed_smoke` | window + swapchain render/present + pacer wait for 3 frames |
| `window_win32_input` | Win32 mouse, wheel, key and character translation into `InputEvent` (Windows only) |
| `batched2d_on_demand` | scene-driven render-on-demand: renders only for startup and scene changes, idles between |
| `batched2d_cpu_golden` | CPU-canvas solid-rect baseline, bit-exact (all platforms) |
| `batched2d_cpu_text_golden` | CPU-canvas text baseline (single-line + wrapped PaintData), bit-exact (all platforms) |
| `batched2d_cpu_vector_golden` | CPU-canvas vector-asset baseline (SVG → CPU raster → textured-quad blit), bit-exact (all platforms; skips when ThorVG is OFF) |
| `batched2d_cpu_image_golden` | CPU-canvas raster-image baseline (PNG → decode cache → textured-quad blit), bit-exact (all platforms) |
| `cpu_wide_golden` | CPU-canvas wider paint corpus: borders, nested clips and linear gradients, bit-exact (all platforms; `--update-goldens` regenerates) |
| `cpu_canvas` | pure CPU coverage-blend math: rounding, alpha, overlap, clip, off-canvas blits, textured-quad sampling/tint/guards |
| `cpu_canvas_corpus` | software-backend primitive corpus: solid quads, pixel-center edges, alpha order, textured quads, 0-size/off-canvas/1-pixel/negative geometry, negative control |
| `cpu_canvas_scene_corpus` | sink-level corpus: nested/disjoint clip stacks, translate vs rotation AABB, rounded-rect bounds gap, damage-scissored replay == full redraw, negative control |
| `image_diff` | RGBA8 exact/tolerant diff, mismatch ratio, size/buffer rejection |
| `merger_cli_help` | merger CLI usage |
| `merger_cli_idempotent` | CLI merge idempotency |
| `automation_sha256` | FIPS 180-4 SHA-256 known-answer vectors (empty, `abc`, 448/896-bit, 1e6×`a`, binary) |
| `automation_hash_id` | salted `hash64` UiIds, explicit `automationId` override, duplicate and collision diagnostics |
| `automation_snapshot` | preorder snapshot order, hidden/offscreen omission, `includeHidden`, `(uiId, itemKey)` lookup |
| `automation_snapshot_json` | canonical one-line JSON, decimal `uiId`s above 2^53, escaping, byte-identical reruns |
| `virtual_geometry` | 32 px estimates, Fenwick prefix sums, refine-on-measure suffix shift, visible range |
| `virtual_geometry_scan` | brute-force window oracle across gaps, fractional offsets, zero-size and tall rows |
| `virtual_anchor` | section 6 re-anchor on `(item_key, offset)` after a mid-list measure |
| `virtual_state` | keyed item state across recycle, eviction rule, no recycled-state leak |
| `virtual_allocations` | allocation-free hot path after warm-up (global `new` counter) |
| `virtual_data_source` | read-only data source contract: build fills a clean caller node, optional release, measure constraints |
| `virtual_key_index` | fixed-capacity key index: O(1) find/key_at, duplicate and capacity errors invalidate the mapping |
| `virtual_pool` | recycler: cap refusal without growth, clean-on-acquire, stale-lease rejection, detach/reclaim |
| `ir_ids` | IR identity assignment (componentId/uiId from salt, source path and node path, explicit automationId, kept ids, errors) and the deterministic automation manifest |
| `parity_runner` | preview-parity runner: `call` and `click` steps, int/float identity, mismatch report naming step, id and cell, run errors |
| `preview_parity` | `samples/parity-demo`: a native handler's TSX preview body (IR evaluator) and its developer C++ leave the same state after every scripted click, addressed by the numeric uiId |
| `virtual_range` | visible-range release/build over the overscan window with re-anchor relative to the first visible item |
| `virtual_range_measure` | async measure refinement through `VirtualRange` with suffix shift and re-anchor |
| `virtual_churn` | 10k items, 1500 scroll steps: zero allocations after warm-up, cap/in-use invariants, determinism |
| `virtual_focus` | keyed focus/selection in the state store, one realized `PseudoState::Focus`, pinned entries |
| `virtual_scroll_index` | `scroll_to_index` Start/Center/Nearest clamps, pure query identity, re-anchor interaction |
| `virtual_focus_churn` | 10k-item focus/step/selection script: allocation parity, cap high-water mark, repeat determinism |
| `virtual_grid_rows` | row-granular grid geometry: row-major mapping, row extent, visible rows/items |
| `virtual_collection_semantics` | set size and 1-based `pos_in_set` from the key index, realized `(UiId, ItemKey)` identity, active descendant, selection in data order |
| `virtual_announce` | bounded coalesced focus/selection announcements: one per change, supersede/drop, no per-frame, allocation parity |
| `frame_pipeline_style_sync` | interpolation sample whitelist and blend at alpha 0/0.5/1 (transform shortest arc, skew hold, drops) |
| `frame_pipeline_overflow` | scripted two-tick scene painted at alpha 0/0.5/1: damage covers the interpolated box, hit test stays at tick geometry |
| `boundary_model` | nearest-catcher routing, fallback, reset, async values, cleanup seam |
| `boundary_retry` | 250/500/1000/2000/4000 ms schedule, five-retry cap, `TESSERA_BOUNDARY_LOOP` |
| `boundary_nested` | nested boundaries and root routing |
| `boundary_guard` | sync catch point, no silent catch, fallback failure contained |
| `codegen_composition` | component composition: host node, `build(parent, slot)` overload, goldens `Card`/`CardPage` |
| `codegen_composition_runtime` | generated component tree: slot order, empty slot, child-first unmount |
| `codegen_virtual` | VirtualList/VirtualGrid emission: geometry constants, window table, 12 rejects, byte determinism |
| `codegen_virtual_runtime` | scripted scroll through `ItemGeometry`: realized range, keyed reuse, Clean/Paint domains, 32-item clamp |
| `codegen_virtual_scroll` | generated `scroll_virtual_<n>`: overscanned range parity, suffix-only measure shift + in-place re-anchor, `onEndReached` edge/throttle (200 px), grid rows, 4096-item cap refusal |
| `forms_lowering` | Field resolves to an ordinary `Component` from a `"tessera"` import: prop-record matrix, Children slot, project-wins, determinism |
| `forms_codegen` | prop/callback emission through `apply_props()`, typed unknown/mis-bound/wrong-type rejections, no-rule-constant audit, goldens |
| `forms_codegen_runtime` | compiled two-Field form: dispatch to the caller's Button, host update repaints Field 1 only, callback bound on the validating Field |
| `forms_form_codegen` | Form rejection matrix: controller-owned Field props, widget-level value/onChange inside a Form, unknown props, hash-guard and callback-once checks |
| `scale_quantize` | render-scale ladder quantization (nearest, ties up, clamped) over the DPI table |
| `scale_state` | per-window render scale: recompute only on DPI/monitor change, revision on step change |
| `scale_atlas` | one glyph atlas per ladder step, no cross-scale reuse, per-step invalidation |
| `harness_framing` | JSON-lines framing: CRLF/EOF, 1 MiB cap, over-long streaming, UTF-8 rejections |
| `harness_transport` | `LineTransport` WouldBlock/Closed/drain rules and the in-process pair |
| `harness_json` | strict minimal JSON: duplicate keys, control chars, surrogates, depth, int64 range |
| `harness_protocol` | versioned envelope, correlation ids, error codes and recovery |
| `harness_command` | snapshot/click/setValue/focus/scroll/trace/audit command decode rules |
| `harness_command_result` | result shapes per command, mismatch rejection, round-trips |
| `harness_golden` | byte-pinned request/response lines, repeat determinism, 256-node payload |
| `harness_stdio_transport` | readiness-probed stdio reads, WouldBlock vs EOF tail, bounded write queue, framing rejections keep stream position |
| `harness_stdio_transport_posix` | real POSIX process stdio over pipes (UNIX only): `poll(2)` readiness and timeout, EOF tail, byte-identical write round trip, EPIPE, full-pipe `WouldBlock` on the non-blocking write (Linux), transport delegation |
| `harness_handler` | host seam dispatch for every v1 command, artifact paths/caps, relative-path validation |
| `harness_session` | bounded session executor: FIFO ordering, back-pressure, per-pump budgets, timeout, ordered shutdown |
| `harness_session_stream` | full round trip including focus and scroll over a stream pair |
| `harness_cli` | CLI argument handling: `--help`/`--version`, mode/flag validation, unreadable/invalid script exits |
| `harness_cli_integration` | promoted CLI: `tessera test/headless --script` exit codes, report/trace/audit artifacts, deterministic reruns |
| `harness_engine_host` | wired lane over a real `AppHost` scene: one script runs snapshot/click/type/focus/scroll/trace/audit with byte-identical reruns |
| `harness_engine_host_failure` | wired failure paths: `unsupportedAction`/`unknownAction`, `uiIdNotFound`, step cap, run/step timeout, non-focusable/`itemKey`/hidden targets |
| `harness_diagnostics_cost` | disabled trace/audit attach no work: empty recorder/registry, no fabricated report, same bytes as no seams |
| `harness_tick_budget` | `SessionExecutor::pump(SessionTickBudget)` narrows one pump to a runtime tick's reads/commands |
| `a11y_uia_mapping` | UIA control types/patterns/automation ids from the semantics tree |
| `a11y_uia_handoff` | single-slot snapshot handoff: overwrite-with-count, reserved buffers, no allocation |
| `a11y_uia_bridge` | MTA bridge: publication, action ring overflow, thread ownership |
| `a11y_uia_bridge_lifecycle` | init/start/stop ordering, init failure as a value, bounded wait with a blocked pump, restart |
| `a11y_uia_provider` | provider projection from the bridge thread: tree, bounds, actions |
| `a11y_uia_com` | Windows-only: real COM provider vtable properties/patterns (Windows only) |
| `a11y_uia_com_nav` | Windows-only: navigation/hit-test/focus/shutdown on the COM provider (Windows only) |
| `a11y_uia_compile_check` | Windows-only: ids static_asserted against the SDK headers and the static link resolves (Windows only) |
| `compiler_limits` | deterministic resource-limit stress corpus: file/token/depth/diagnostic/component bounds, excerpt caps, determinism |
| `jsx_diagnostics_registry` | every stable `TESSERA_JSX_*`/`TESSERA_REACT_*` code pinned by a fixture or explicit skip (59 codes / 53 fixtures / 6 skips) and frozen in `tests/golden/jsx/diagnostics-registry.txt` |
| `jsx_diagnostics_registry` | diagnostic-code registry: one minimal fixture per analyzer code (code, byte span, line:column, bounded message), source-emission drift check, explicit skip list, byte-compared golden |
| `icu_locale_pack` | reduced data blob installs via `udata_setCommonData`; pinned version, root bundle, no shared ICU loaded in-process |
| `text_breaks_parity` | UAX #29 word / UAX #14 line breaks vs the builtin reduced-rules backend over the wrap fixture corpus |
| `locale_resolver` | manifest-validated BCP-47 negotiation, ancestor fallback, direction, `icu.locale.*` diagnostics |
| `manifest_parse` | shader manifest shape load: valid per kind, model fields, defaults |
| `manifest_limits` | 1 MiB / depth / count caps enforced before decode |
| `manifest_keys` | closed key sets, required keys, wrong types, duplicate keys, trailing content |
| `manifest_stages` | stage/entry rules, compute rejection, unknown/duplicate stage, version mismatch |
| `manifest_errors` | stable error tokens/paths/offsets, repeated-parse determinism, loader failures |
| `manifest_build_step` | `tools/shaders/manifest.json` loads; a broken copy yields the typed error; batched2d layout/bindings/push constants match Vertex2D and the GLSL |
| `a11y_dbus_address` | session-bus address parse: percent decoding, unix path/abstract, unsupported transports |
| `a11y_dbus_transport` | `Transport` seam + loopback pair rules (no sleeping, bounded buffers) |
| `a11y_dbus_wire` | wire codec golden bytes: alignment table, signatures, header fields, BE input |
| `a11y_dbus_wire_bounds` | caps (text/field/body/nesting) and malformed/truncated message rejection |
| `a11y_dbus_auth` | SASL EXTERNAL handshake: NUL + hex uid, OK/REJECTED/ERROR/DATA, fd-passing fallback |
| `a11y_atspi_mapping` | AtspiRole table, 2-word state set, action names, accessible object paths |
| `a11y_atspi_events` | bounded coalescing event queue keyed by (kind, node, detail) |
| `a11y_atspi_bridge` | Accessible/Application/Component dispatch and error replies |
| `a11y_atspi_bridge_events` | Action/Text/Value dispatch, deferred action requests, one signal per event |
| `a11y_atspi_session` | handshake byte replay: SASL, Hello, GetAddress, a11y bus, RegisterEvent, serial dispatch |
| `a11y_atspi_session_pump` | worker start/stop join, restart, zero-budget timeout, close before/after Ready, determinism |
| `a11y_atspi_properties` | read-only `Properties` Get/GetAll typing/order, InvalidArgs/UnknownMethod, malformed body |

## 2.1 GPU tests

GPU tests live under `tests/gpu/` and are registered in `tests/gpu/CMakeLists.txt`:

```text
tests/gpu/{rhi,resources,pipelines,frames,backends,shaders,batched2d}/
tests/integration/gpu/  tests/golden/gpu/  tests/fixtures/gpu/  tests/benchmarks/gpu/
```

Use the null backend for command/validation structure and lavapipe/software for pixel goldens.
Real GPUs are smoke/tolerance only. See [GPU testing](../design/gpu/testing.md).

## 3. Add a unit test

1. Create `tests/unit/<component>_tests.cpp`.
2. Use the existing style: a `main()` returning nonzero on failure; print a clear message.
3. Add to `tests/unit/CMakeLists.txt`:

```cmake
add_executable(my_component_tests my_component_tests.cpp)
target_link_libraries(my_component_tests PRIVATE tessera_my_component)
add_test(NAME my_component COMMAND my_component_tests)
```

4. Keep tests deterministic: no wall clock, network, system fonts, or random ids.
5. Run `ctest -R my_component --output-on-failure`.

A subsystem may keep its registration in `tests/unit/<area>/CMakeLists.txt` (with one
`add_subdirectory(<area>)` in `tests/unit/CMakeLists.txt`) when several slices are developed at the
same time: the root list stays the single index while each slice owns its own file. Existing areas:
`automation/`, `virtual/`, `boundary/`, `a11y/`, `harness/`, `manifest/`, `scale/`.

## 3.0 Headless agent tests

These commands are the target contract from [headless-automation-v1](../specs/headless-automation-v1.md); the `tessera test` and `tessera headless` commands are not implemented yet.

```bash
tessera test --script tests/agent/home.json --report out/report.json
tessera test --script tests/agent/scroll.json --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/home.json
```

- Use numeric `uiId`/`(uiId,itemKey)` from `generated/automation.manifest.json`, not coordinates.
- Structural mode (null backend) is fast; golden mode uses the software path (lavapipe or the CPU rasterizer).
- Assertions: visibility/text/focus/bounds/events/no boundary catches/golden/audit thresholds.
- Outputs: `report.json`, `snapshot.json`, `trace.json`, `audit.json`.
- Custom elements: unit fake context, structural draw stats, golden offscreen shaders, GPU
  smoke; use `custom`/`assertCustom` and `partId` for sub-targets.
- Interaction suites: text editing/IME, scroll/gestures, overlays, navigation, forms, and
  accessibility (semantics snapshots + real screen readers for platform smoke).
- See [`agent-automation.md`](agent-automation.md), [`custom-elements.md`](custom-elements.md),
  and [`headless-automation-v1.md`](../specs/headless-automation-v1.md).

## 3.1 Error-boundary tests

- Throw in child build/update/effect/event -> fallback shown, error logged (see [error-boundaries-v1](../specs/error-boundaries-v1.md)).
- Nested boundary: nearest catches; fallback failure propagates to parent/root.
- Reset/retry/resetKey; async error report; cancellation does not show fallback.
- No silent catch; redaction; no throw across threads/FFI; loop protection.

## 4. Golden tests (lavapipe harness)

Implemented for the batched2d proof:

- `batched2d_golden` is registered only on Linux when CMake finds an installed lavapipe ICD
  (`lvp_icd*.json`). It renders with `--golden` and compares
  `tests/golden/baseline/batched2d/dashboard.png` bit-exactly
  (`--tolerance 0 --max-mismatch 0`).
- `batched2d_overlay_golden` renders `--overlay --font <fixture>` and compares
  `tests/golden/baseline/batched2d/dashboard_overlay.png` with `--tolerance 1 --max-mismatch
  0.001`. Glyph antialiasing rounds differently across lavapipe/LLVM revisions, so text-bearing
  cases allow one channel level and a 0.1% pixel budget; structural regressions still fail.
- `batched2d_text_golden` covers the text stack (font loaded, shaped and rasterized) against
  `tests/golden/baseline/batched2d/dashboard_text.png` with the same tolerance.
- `batched2d_shapes_golden` renders `--shapes`: rounded cards, two UV-scale textures, scissor
  clipping, translation, and a combined clip+transform strip against
  `tests/golden/baseline/batched2d/dashboard_shapes.png` bit-exactly (`--tolerance 0
  --max-mismatch 0`).
- `batched2d_cpu_golden` renders the CPU-canvas solid-rect dashboard and compares
  `tests/golden/baseline/batched2d/dashboard_cpu.png` bit-exactly on every platform (no GPU).
- `batched2d_cpu_text_golden` renders the CPU text scene through the paint pass
  (`--backend cpu --cpu-text --font <fixture> --width 320 --height 160`): one `PaintData`
  single-line title and two wrapped `PaintData` paragraphs, composited from FreeType glyph
  coverage by `CpuCanvas::blend_coverage` with the documented integer straight-alpha source-over
  math. It compares `tests/golden/baseline/batched2d/dashboard_text_cpu.png` bit-exactly
  (`--tolerance 0 --max-mismatch 0`), so text goldens run on the CPU path in CI with no GPU or
  lavapipe. The CPU path is the deterministic reference; GPU text cases (`batched2d_text_golden`,
  `batched2d_overlay_golden`) keep their 1-LSB/0.1% tolerance because GPU blending rounds in
  float before UNORM conversion.
  Regenerate deliberately (never from CI):
  `batched2d_poc --backend cpu --cpu-text --font tests/fixtures/fonts/NotoSans-Regular.ttf
  --text "Tessera CPU text" --text-size 20 --width 320 --height 160 --out
  tests/golden/baseline/batched2d/dashboard_text_cpu.png`, inspect the PNG, then commit it with
  the behavior/test change. Two CPU runs must be byte-identical before the baseline is accepted.
- `batched2d_cpu_vector_golden` is a Vulkan-free executable (`tests/gpu/cpu_vector_golden_tests.cpp`,
  registered from `src/ui/paint/CMakeLists.txt`) that renders the same scene as
  `batched2d_poc --backend cpu --cpu-vector --width 320 --height 160`: embedded SVG assets painted
  through `PaintKind::Vector` are decoded, CPU-rasterized and composited
  by `CpuCanvas::blit_texture` (one textured quad per asset, tint + clip, documented integer
  nearest-sample/source-over math). The scene covers two nodes sharing one icon, a tinted/translucent
  node, a node clipped by an `overflow: hidden` parent, and a missing asset that must stay
  background. It compares `tests/golden/baseline/batched2d/dashboard_vector_cpu.png` bit-exactly
  (`--tolerance 0 --max-mismatch 0`). The test is registered always but skips cleanly with
  `SKIP_REGULAR_EXPRESSION` when the build has ThorVG OFF; Linux CI configures with
  `-DTESSERA_ENABLE_THORVG=ON` so it runs. Regenerate deliberately (never from CI):
  `batched2d_poc --backend cpu --cpu-vector --width 320 --height 160 --out
  tests/golden/baseline/batched2d/dashboard_vector_cpu.png`, inspect the PNG, confirm two runs are
  byte-identical, then commit it with the behavior/test change. Contract and measurements:
  [design/thorvg-spike/11-cpu-vector-golden.md](../design/thorvg-spike/11-cpu-vector-golden.md).
- `batched2d_cpu_image_golden` is a Vulkan-free executable
  (`tests/gpu/cpu_image_golden_tests.cpp`, registered from `src/ui/paint/CMakeLists.txt`) that
  renders the shared raster-image scene (two nodes sharing one embedded PNG, a tinted/translucent
  node, a node clipped by an `overflow: hidden` parent, a missing asset that must stay background),
  asserts a byte-identical repeat and one decode for all nodes, and compares
  `tests/golden/baseline/batched2d/dashboard_image_cpu.png` bit-exactly (`--tolerance 0`). PNG only:
  JPEG decode uses floating point whose last bits may differ across compilers, so JPEG is covered by
  unit decode tests and the Vulkan readback test instead. Regenerate deliberately:
  `batched2d_poc --backend cpu --cpu-image --width 320 --height 160 --out
  tests/golden/baseline/batched2d/dashboard_image_cpu.png`, inspect the PNG, confirm two runs are
  byte-identical, then commit it with the behavior/test change. Contract and measurements:
  [design/thorvg-spike/12-image-assets.md](../design/thorvg-spike/12-image-assets.md).
- `cpu_wide_golden` is a Vulkan-free executable (`tests/gpu/cpu_wide_golden_tests.cpp`, registered
  from `tests/gpu/CMakeLists.txt`) that paints three scene graphs through `ui::paint::paint_scene`
  and `CpuPaintSink`/`CpuCanvas`: `borders` (four differently sized/coloured border sides, a
  uniform square border, and a borderless rounded card that pins the documented bounds
  approximation), `clips` (three nested `overflow: hidden` clips, an outer clip that starts
  off-canvas, and a disjoint clip whose child must paint nothing), and `gradients` (a vertical
  three-stop and a horizontal two-stop linear ramp sampled by the sink's shared
  `sample_linear_gradient`, plus a gradient cut by two nested clips). It asserts a byte-identical
  repeat, then compares `tests/golden/baseline/cpu_wide/{borders,clips,gradients}.png` bit-exactly
  (tolerance 0, zero mismatched pixels). Regenerate deliberately from the CPU path only:
  `./build/<dir>/tests/gpu/cpu_wide_golden_tests --update-goldens`, inspect the PNGs, confirm a
  second run changes no byte, then commit them with the behavior/test change.
- `batched2d_windowed_smoke` creates a hidden platform window, a Vulkan swapchain, renders the
  dashboard into acquired images, presents three frames, and uses `wait_for_next_frame` for
  pacing while `RuntimeLoop` decides whether each iteration renders. The test requires the
  output line `rendered=3`, so a run that renders nothing fails instead of passing silently. It runs under Xvfb on Linux and on Windows; it skips cleanly when no window backend or
  presentation surface exists.
- `batched2d_on_demand` runs `batched2d_poc --on-demand`: a `Node` scene driven through
  `FramePipeline` and a render-on-demand `RuntimeLoop`. Scripted scene changes stand in for input;
  the run must render the startup frame and one frame per change (`rendered>=3`), render nothing
  without a cause (`unjustified=0`), lose no change (`missed=0`) and park on the window wait in
  between (`idle_waits>=1`). `PASS_REGULAR_EXPRESSION` ignores the exit code, so the expression
  carries the whole check.
- `batched2d_cpu_golden` exercises `batched2d_poc --backend cpu`: a CPU canvas fills
  axis-aligned solid rectangles into a separate dashboard baseline
  (`dashboard_cpu.png`), bit-exactly. The software backend's wider corpus is documented in
  section 4.1.

### 4.1 Software (CPU canvas) corpus

`CpuCanvas` (`src/rendering/software/`) is the deterministic software reference used by the CPU
goldens (`dashboard_cpu.png`, `dashboard_text_cpu.png`, `dashboard_vector_cpu.png`,
`dashboard_image_cpu.png`) and by the wider `tests/golden/baseline/cpu_wide/` set. Its contract,
and the exact list of what it does and does not draw, lives in
[golden-tests-v1.md, section 1.2](../specs/golden-tests-v1.md#12-software-cpu-canvas-corpus).
The two Vulkan-free suites next to `cpu_canvas` pin it on exact RGBA8 bytes, so widening the
backend cannot silently change pixels:

- `cpu_canvas_corpus` covers the primitives themselves: solid quads and the pixel-center edge rule
  (integral/fractional/half-pixel/1-pixel), overlapping translucent quads (draw order is
  significant), textured quads (nearest sampling, tint, clip, negative origin), and boundary
  geometry (zero-size, off-canvas, negative coordinates, 0x0 and 1x1 canvases). A deliberately
  shifted draw and a wrong expected pixel prove the assertions can fail.
- `cpu_canvas_scene_corpus` covers the sink-level mapping through the real paint pass: nested
  `overflow` clip stacks (the visible region is the three-way intersection), disjoint clips (an
  empty intersection paints nothing), translate-only transforms (exact), rounded-rect backgrounds,
  and damage-scissored replays (`ClippedPaintSink` per disjoint region must equal a full redraw
  byte for byte, with an undersized-region negative control).
- `cpu_wide_golden` commits the wider paint corpus as image baselines: `borders` (all four border
  sides with distinct widths and colours over a rounded background, a uniform square border, and a
  borderless rounded card that pins the bounds approximation), `clips` (three nested clips, a clip
  that starts off-canvas, and a disjoint clip that must paint nothing), and `gradients` (vertical
  and horizontal linear ramps plus a gradient cut by nested clips). The three PNGs under
  `tests/golden/baseline/cpu_wide/` are generated on the CPU path only and compared at tolerance 0,
  so they must be byte-identical on Windows and Linux.

Documented gaps, kept explicit instead of faked: no rounded/SDF rasterizer (a rounded rect paints
its bounds, corners stay square), no rotation/scale/skew rasterizer (a transformed node paints its
world-bounds AABB; translate is exact), gradients approximated as 64 dominant-axis bands, box
shadows as stacked rect layers, no clip stack inside the canvas (clipping is a per-call rect from
the sink), nearest-sample only (no resampling filter), and no geometric AA (glyph coverage is the
exception). Rounded, rotating, and resampled shapes stay on the lavapipe corpus
(`batched2d_shapes_golden` and the text cases).

Both suites are deterministic and fast: no GPU, lavapipe, font, clock, random id, or network.
- `--report report.json` and `--diff diff.png` write structural failure artifacts for any
  comparison: status, dimensions, mismatch counts, first mismatch coordinate, and a
  magenta-on-gray diff image when per-channel deltas exceed the tolerance.
- Generate or update a baseline only through lavapipe:

  ```bash
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
    ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --golden \
    --out tests/golden/baseline/batched2d/dashboard.png
  ```

  Review the image as a separate, explicit baseline-update change.
- `batched2d_poc --compare golden.png [--tolerance N] [--max-mismatch R]` also supports tolerant
  GPU smoke checks; GPU runs never update baselines.
- `rendering/golden/image_diff.hpp` compares RGBA8 images and reports mismatched pixels, max/mean
  channel delta, and the first mismatch.
- Not implemented yet: a golden corpus beyond batched2d. See
  [`golden-tests-v1.md`](../specs/golden-tests-v1.md) and
  [`design/testing.md`](../design/testing.md).

### 4.2 Backend-selectable harness, tolerances and the conformance summary

The GPU harness selects an RHI backend at run time, keeps per-golden tolerances in
[`tests/golden/tolerance.json`](../../tests/golden/tolerance.json), and reports a conformance
summary per backend (issue #161). Goldens still come from lavapipe/CPU only: the conformance
runner checks the RHI contract, it never compares pixels and never writes or updates a baseline.

**Runner.** `tests/gpu/conformance/` builds `gpu_conformance`:

```text
gpu_conformance [--backend null|vulkan] [--summary <path>]
```

- `--backend` selects the device (default `null`). `null` is always available; `vulkan` needs a
  build that includes the Vulkan backend and a device on the host.
- `--summary` writes the deterministic JSON summary. Exit codes: `0` when every check passes or
  skips, `1` when a check fails, `2` for a usage or I/O error.
- A backend that cannot open a device prints `gpu_conformance: skipped (<reason>)`, still writes
  an all-skip summary and exits `0`, so CI without a GPU stays green. The
  `gpu_conformance_vulkan` ctest entry turns that line into a skipped test through
  `SKIP_REGULAR_EXPRESSION`.

Each check is `{name, capability, run(Device&)}` (`conformance_check.hpp`). The suite sorts checks
by name and applies the capability gate: when a backend lacks a capability, the check is skipped
with `missing capability: <capability>` instead of failing. Checks cover device creation and the
reported backend, buffer round-trip, buffer out-of-range rejection, render-target/texture/pipeline
creation, command-list record and submit, `DeviceStatus::Ready`, and the GPU-timestamp probe
(`timestamp_capability_honest`, plus `timestamp_query_roundtrip`, which skips when
`GpuTimestampCapability::usable()` is false). Resource sizes stay at or below 64x64.

**Summary.** Byte-identical for two runs on the same backend: check names sorted, fixed key order,
LF newlines, and no timestamps, pointers or absolute paths. Schema 1:

```json
{
  "backend": "null",
  "checks": [
    {
      "capability": "device",
      "detail": "",
      "name": "device_create",
      "status": "pass"
    }
  ],
  "schema": 1,
  "totals": {
    "fail": 0,
    "pass": 9,
    "skip": 1,
    "total": 10
  }
}
```

`status` is `pass`, `skip` or `fail`; `detail` is empty for a pass, the skip reason for a skip
(`missing capability: ...`) and the failure message for a fail. When a check throws, the driver
text goes to stderr and the summary records a fixed message, so the artifact stays
machine-independent. `tests/gpu/conformance/conformance_summary_tests.cpp` runs the null suite
twice and asserts byte equality, a non-empty sorted check list, and pass-or-skip only.

**Tolerances.** `tolerance.json` replaces the numbers hard-coded in golden registrations:

```json
{
  "schema": 1,
  "defaults": {"tolerance": 0, "max_mismatch": 0.0},
  "tests": {
    "batched2d_overlay_golden": {"tolerance": 1, "max_mismatch": 0.001},
    "batched2d_text_golden": {"tolerance": 1, "max_mismatch": 0.001},
    "harness_frame_capture": {"tolerance": 1, "max_mismatch": 0.001}
  }
}
```

`tolerance` is the per-channel 8-bit delta a pixel may still count as matching; `max_mismatch` is
the fraction of pixels (0.0-1.0) allowed to exceed it. Names are ctest names; an unlisted golden
falls back to `defaults`. [`scripts/golden_tolerances.py`](../../scripts/golden_tolerances.py)
validates the file strictly and prints sorted `name<TAB>tolerance<TAB>max_mismatch` lines for
CMake (`--json` for the canonical document, `--name <test> --field tolerance|max_mismatch` for a
single value). Wiring one golden test:

```cmake
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/golden_tolerances.py"
            --name batched2d_overlay_golden --field tolerance
    OUTPUT_VARIABLE BATCHED2D_OVERLAY_TOLERANCE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE result)
if (NOT result EQUAL 0)
    message(FATAL_ERROR "golden_tolerances.py failed for batched2d_overlay_golden")
endif()
```

then pass `--tolerance ${BATCHED2D_OVERLAY_TOLERANCE}` (and the matching `--max-mismatch`) to the
golden command. Two entries are still compiled into their executables (`cpu_wide_golden` stays
bit-exact and `harness_frame_capture` keeps 1 LSB + 0.1%); their JSON entries mirror those values
until the executables accept a CLI override.

## 5. Deterministic mode

- Fixed timestep and simulated clock.
- Synthetic input/OS events (resize, DPI, fold, power, IME).
- Fixed locale/time zone/ICU data.
- Committed fonts/assets; no system lookups.
- Single-threaded/deterministic executor; no detached tasks.
- No network; filesystem limited to fixtures.

## 6. Sanitizers and validation

- ASan/UBSan (address and undefined-behavior sanitizers): unit/integration tests on toolchains that support them (not yet wired into CI, see section 9).
- TSan (thread sanitizer): async/threading paths.
- Vulkan validation layers: mandatory in debug builds; CI may disable them only if lavapipe cannot provide
  the layer.
- clang-tidy/format: CI gates.

## 7. Benchmarks

- Benchmarks are opt-in and not shipped.
- Measure frame stages (style/layout/paint/render), allocation counts, cache hit rates, GPU
  memory.
- A static scene must produce zero presents/submissions after settling.
- Record baselines and compare with a tolerance (value not specified in any doc); do not gate on noisy wall-clock numbers.
- `tests/benchmarks/cpu/` (CTest `cpu_benchmark`, issue #241) is the V1 timing evidence: it runs in
  the default suite with order-of-magnitude budgets, so a regression fails a normal `ctest` run.
- `tests/benchmarks/stylesheet/` (CTest `stylesheet_startup_benchmark`, label `benchmark`, issue
  #201) is counter-only and opt-in: one stylesheet through the embedded-blob path and the run-time
  parse+compile path, compared against `tests/golden/perf/stylesheet_benchmark.json` (structural
  counters exact, allocation counters within `tolerance.allocation_percent`). The label is the
  opt-in handle and `DISABLED` gates it, so the default suite reports `Not Run (Disabled)`. Run it
  with `-DTESSERA_BENCHMARK_STYLESHEET=ON` and `ctest -L benchmark`.

## 8. Fuzzing

- CSS tokenizer/parser, JSX/TSX parser, IR validation, manifest/IPC parsers, asset decoders.
- Bounded inputs/timeouts; crashes are failures.
- Run on a schedule, not on every PR (schedule not specified; no fuzz job exists in `.github/workflows/ci.yml`).

## 9. CI

Current Linux workflow:

1. install toolchain + lavapipe + `clang-format==22.1.3`
2. format check
3. configure/build (`ci` preset)
4. static C++ runtime check
5. ctest with lavapipe
6. Vulkan offscreen smoke + artifact

A Windows CI job does not exist yet (`.github/workflows/ci.yml` has only the Ubuntu job); it should mirror these steps with the VS preset.

## 10. Failure triage

- Reproduce with the exact preset/backend/ICD.
- For GPU-only failures, run with validation layers and lavapipe.
- For async failures, run TSan and reduce to a deterministic script.
- For golden failures, inspect `report.json` + diff before updating anything.
- Add a regression test before or in the same change as the fix.
