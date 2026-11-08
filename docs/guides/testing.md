# Testing guide

Status: the tests of Big Step B1 (core renderer) run with CTest; golden tests (deterministic reference images) belong to Big Step B2 (see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step); Mesa lavapipe is a software Vulkan driver, see [ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md), deterministic headless golden tests). The target test pyramid is in [design/testing.md](../design/testing.md).

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
| `frame_pacer` | uncapped/deadline waits, lateness, cadence reset, validation |
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
| `merger_cli_help` | merger CLI usage |
| `merger_cli_idempotent` | CLI merge idempotency |

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

## 4. Golden tests (target)

- Use the software path (lavapipe or the CPU rasterizer), with fixed size, scale, fonts, clock and events.
- Compare bit-exact; update baselines only with an explicit reviewed command.
- Failure artifacts: image diff + `report.json` (node bounds/colors/dirty).
- See [`golden-tests-v1.md`](../specs/golden-tests-v1.md) and
  [`design/testing.md`](../design/testing.md).

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
