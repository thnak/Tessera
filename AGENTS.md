# AGENTS.md — Tessera

Working name. This file is the operating manual for any coding agent (and for humans)
working in this repo. Read it before touching code. Contribution workflow:
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## What this project is

A cross-platform C++ UI framework:

- **DSL**: standard React/JSX syntax (`useState`, `useEffect`, `useRef`, `useAnimatedValue`).
- **Production**: a two-tier compiler turns JSX → IR → **native C++** source.
- **Design time**: the same C++ engine compiled to **WebAssembly + WebGPU** for instant,
  emulator-free preview in a browser.
- **Web target (secondary)**: the same app, generated C++ plus developer C++, built with
  Emscripten to run in a browser for demos and trials
  ([ADR-046](decisions/ADR-046-web-target-for-demos.md)); native stays primary.
- **Native backend**: **Vulkan 1.3 is the primary renderer**. WebGPU/Dawn is the browser backend.
- **Core model**: game-engine-style fixed timestep, zero-allocation buffers, retained/immediate
  hybrid, dirty-flag invalidation.
- **AI-friendly**: deterministic offscreen framebuffer + golden-image diff, usable by agents.

## Non-negotiables

1. **Vulkan 1.3 native, WebGPU for WebAssembly.** Never attempt to run Vulkan inside Wasm.
   Two backends, one thin RHI, one shared golden-test corpus.
2. **Buy before build.** Do not re-implement solved infrastructure:
   - Layout: **Yoga** (Flexbox). Grid: small subset or taffy.
   - Vector paths: **ThorVG** or **Skia Graphite** (Vulkan). Never write a path rasterizer in V1.
   - Text: **FreeType + HarfBuzz + SheenBidi + ICU**.
   - GPU memory: **VMA**. Loader: **volk** / `vk-bootstrap`.
   - Browser GPU: **Dawn** (WebGPU).
   The canonical list, with license and module ownership, is
   [`docs/specs/dependencies.md`](docs/specs/dependencies.md). Add every new dependency there.
3. **Thin RHI.** `src/rendering/rhi/` is the only seam between UI/rendering and a GPU API.
   Backends live under `src/rendering/backend/{vulkan,webgpu,null}`.
4. **Compiler is IR-first.** JSX/TSX → `src/compiler/ir` → C++ codegen. Never make codegen
   read the raw parser AST directly.
5. **Generated code and developer code have separate owners** ([ADR-040](decisions/ADR-040-generated-base-and-developer-class.md)).
   Codegen emits a `gen::<Component>Base<Self>` that is never edited or merged; developer C++
   lives in a developer-owned derived class scaffolded once, and a missing `bind::` member is a
   compile error. The migration is under way: #209 and #210 are done, #218 moves the codegen
   test families to the base one by one (sample and dynamic are done), and #219 deletes the legacy
   output, the merger and `EmitMode`. Until #219 lands the legacy merger must stay idempotent
   (`merge(merge(g, e), e) == merge(g, e)`); add no new features to it.
6. **Deterministic headless tests use a software path.** Golden images come from
   Mesa **lavapipe** or the CPU rasterizer, never from a real GPU framebuffer.
7. **No React runtime in production.** Production is pure C++. React semantics exist only
   at design time (live view in the Wasm viewer, ADR-036). The model is React-like, not
   React-identical; the compiled UI is "UI-like" and parity with React + browser is a goal,
   not a guarantee.
8. **V1 scope discipline.** Support a small, explicit hooks subset and reject unsupported
   patterns loudly. Do not grow the surface before Big Step C is proven.
9. **Static linking is a hard requirement** (ADR-012). Tessera and third-party C/C++ libraries
   are linked statically into the app. System components (Vulkan loader/ICDs, OS frameworks)
   stay dynamic. No LGPL/GPL in statically linked code.
10. **Project license is MIT** (ADR-013). Keep shipped dependencies permissive; preserve
    third-party notices. Every new dependency needs an entry in
    [`docs/specs/dependencies.md`](docs/specs/dependencies.md) and
    [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
11. **Power-aware by default** (ADR-019). This is an application framework, not a benchmark.
    Static UI must idle (no presents/GPU work), the loop must never busy-wait, and adaptive/
    on-demand defaults are required. Full-GPU utilization is not a goal.
12. **Never block the UI thread** (ADR-021). Use async/await plus bounded state machines; no
    `vkWaitForFences`/`vkDeviceWaitIdle`, blocking I/O, or indefinite acquire/present on the UI
    thread. No detached tasks. Waiting between ticks on the platform event/timer wait (bounded by
    the next deadline and woken by input, posted completions, or a frame request) is allowed; it
    is idling, not blocking in the sense of this rule.
13. **OS compliance lives in the HAL/runtime** (ADR-020). Styles/animations/components see
    `WindowMetrics`, never window handles, OS messages, or swapchains. Resize/DPI/lifecycle are
    normalized and tested per platform.
14. **Diagnostics are structured, bounded, and optional** (ADR-022). No printf-only debugging;
    logs/traces/metrics/crash/capture are privacy-safe and cheap when disabled.
15. **Native-first boundaries; no production bridges** (ADR-024). The UI/render/input/memory
    hot path is direct C++. No JS/React/scripting runtime, virtual DOM, per-frame FFI/IPC, or
    UI objects across JNI/ObjC/Wasm. Bridges are only at OS/build-time/design-time edges; see
    [`docs/design/bridges.md`](docs/design/bridges.md) and
    [`docs/specs/memory-model-v1.md`](docs/specs/memory-model-v1.md).
16. **Error boundaries are the catch point** (ADR-025). Catch synchronous exceptions from
    generated component code at a boundary; async errors are values routed to the owning
    boundary; never throw across threads/FFI. No silent catch; fatal/OOM/cancellation/platform
    errors go to the root handler. See
    [`docs/specs/error-boundaries-v1.md`](docs/specs/error-boundaries-v1.md).

## V1 platform scope and CI policy

Owner decision of 2026-11-13:

- **V1 targets Windows and Linux.** macOS, Android and iOS, and what only they need (the Metal
  backend, fold and posture backends on Android, mobile integration, native touch shells), are not
  V1. They stay open for the release after V1 and are not planned in this file.
- **Gating lanes:** a change is done when it builds and `ctest` passes on Windows (local) and on
  Linux (local or the Linux CI lane), and the Windows, Linux and WebGPU/Wasm CI lanes are green
  (WebGPU is Tier 1, ADR-049). Issue farming notes that say "all six CI lanes" mean these three
  gating lanes.
- **Advisory lanes:** the macOS, Android and iOS CI lanes (and any other lane added later) are nice
  to have and run nightly and on demand, not on every push. A red advisory lane is noted in the PR
  or issue but never blocks a merge or the next slice, and is not fixed ahead of V1 work unless the
  fix is trivial. Do not add platform-specific code only to keep an advisory lane green.
- **Docs-only changes** (`docs/**`, `decisions/**`, any `*.md`) skip the build lanes and run only the
  light `Docs` workflow, which runs every repository gate (`scripts/gates.py`) without a compiler.
- **Library:** component library stages 0 and 1 ship in V1
  ([ADR-032](decisions/ADR-032-bundled-component-library.md)); stages 2 to 6 follow V1.

## Execution loop (default operating mode)

Work as a continuous implementation loop, not a stop-and-wait assistant. A clone can ship the
same idea in parallel; velocity with a green `main` is the advantage.

1. **One Big Goal per cycle.** Pick the next unblocked item from
   [Big Steps](#big-steps-group-work-dont-micro-step) per the [farming protocol](#farming-protocol).
   Write the acceptance in one sentence.
2. **Split the part into independently testable slices.** A slice is one behavior, its tests, and
   its docs. Slices organize the work and the local commits; they are not the unit that lands (see
   [Landing size and CI load](#landing-size-and-ci-load)). Do not bundle unrelated cleanup.
3. **Implement -> test -> format -> commit locally.** Run the format check on changed files, build
   with `TESSERA_WARNINGS_AS_ERRORS=ON`, and `ctest`. Commit locally when green.
4. **Commit locally as often as useful, push once per landing.** Land model -> algorithm ->
   integration -> tests/docs as separate local commits if that helps review, but push only when the
   whole landing is done and verified. Never end a cycle with uncommitted green work.
5. **Continue automatically.** After a commit, take the next slice from the plan; do not stop to
   ask "what next". Report the batch at a natural boundary.
6. **Stop only for:** a destructive/irreversible action, a genuinely ambiguous product decision,
   an external CI failure that needs credentials, or context exhaustion. On context exhaustion,
   commit the current green slice and write the exact next slice in Current focus.

A commit is a checkpoint, not a finish line, and a push is not a checkpoint: it starts CI. Keep
the tree buildable at every commit.

### Farming protocol

How unattended agents pick and land work. Milestones live on GitHub only (no other tracker).

- **Pick:** the first unchecked item of the lowest-numbered open milestone in
  [Big Steps](#big-steps-group-work-dont-micro-step) that is not blocked by an open ADR PR and is
  not already claimed by an open PR (`gh pr list --search "<item words>"`).
- **Blocked by ADR:** ADR-049 and ADR-050 are Accepted (2026-11-12), so their items are open. If an
  item depends on a Proposed ADR, wait until it is Accepted on `main`.
- **One landing per PR**, branched from `origin/main`: a whole issue, a whole family or step of a
  multi-part issue, or several related issues together (`Closes #a, #b`). Attach the milestone
  (`gh pr create --milestone "V1-M1 Core runtime"`) and tick each item's checkbox in this file in
  the same PR (`[x]` plus the evidence path). Size rules are in the next subsection.
- **Format:** check only changed files
  (`git diff --name-only origin/main -- '*.cpp' '*.hpp' | xargs clang-format --dry-run -Werror`);
  do not run the whole-repo format script.
- **Green:** a local Windows build with warnings as errors plus `ctest` is enough to proceed; never
  push to `main` directly and never merge without green tests.
- **Ambiguity:** write the question into the PR description and keep going with the most
  conservative reading; stop only for destructive or irreversible actions.
- **Issue labels** (each roadmap item has a GitHub issue in its milestone):
  `ready` = startable now, `in-progress` = claimed by an open PR or agent, `blocked` = the farm
  could not proceed and the owner must decide or fix something. An issue with none of the three
  waits on an earlier item named in its body; flip it to `ready` when the PR that unblocks it
  merges. Pick `ready` issues in milestone order and swap the label to `in-progress` when you start.
- **Marking a task blocked:** when an item cannot be finished (a missing decision, a spec gap, a
  failing dependency, a platform you cannot test), do not guess and do not leave it half done.
  Comment on the issue with the exact blocker, what you tried, and the smallest question or fix
  that would unblock it; swap `in-progress` or `ready` for `blocked`; keep any green partial work
  as a PR only if it stands alone; then take the next `ready` item. The owner clears `blocked`
  after answering.


### Landing size and CI load

CI is the scarce resource: every push starts the gating lanes, and many small pushes queue behind
each other and starve the farm. Agents therefore land big, complete, pre-verified batches.

- **Land a complete body of work.** A PR is a whole issue (or a whole family or step of a
  multi-part issue such as #218), not a slice of one. Do not open a PR that changes only a few
  files or under roughly 300 changed lines unless it completes its issue; when it would, take the
  next `ready` issue in the same milestone or subsystem on the same branch and land them together.
- **Group by area.** When several `ready` issues touch the same files or subsystem (the same
  `CMakeLists.txt`, the same IR or front-end files, one milestone), do them in one branch and one PR.
  Unrelated areas stay in separate PRs so a failure in one does not hold the other.
- **Never push work in progress**, and never push docs-only, format-only, comment-only or
  rename-only changes by themselves: fold them into the next landing. The one exception is a fix for
  a red `main`.
- **Verify before the push, once.** Build with warnings as errors and run the full `ctest` of the
  affected area on Windows (and on Linux when the change is platform-sensitive or cheap to check)
  plus the format check on changed files, so CI runs a single time. Do not push to find out whether
  CI passes, and do not re-push or re-run only to retrigger CI.
- **One landing in flight.** While a PR's CI runs, keep working locally on the next landing in a
  separate branch; push it only after the previous PR has merged (or been fixed after red).
- **Fix red CI in one push.** Collect every failure of one CI run and fix them together; never
  push a fix per failing test.
- **Local commits stay cheap.** A landing may hold many local commits (model, algorithm,
  integration, tests/docs); they cost nothing until pushed. Prefer a squash-merge so `main` keeps
  one commit per landing.

### Subagent lanes

The Lead may fan a cycle out to subagents when the remaining slices are independent. Delegation is
the default for speed, not the exception: the Lead splits, hands off minimal work, and integrates.

- **Lead splits, subagents execute.** The Lead picks the minimal self-contained slice, writes down
  the behavior, the exact files it may touch, a dedicated build directory, and the acceptance
  test, then hands it to one subagent. One writer per file or directory; never two subagents in
  the same files or in the shared index files (`tests/unit/CMakeLists.txt`, root
  `CMakeLists.txt`, `docs/README.md`) unless the Lead pre-registers those lines.
- **Subagents build and test locally** in their own `build/lane-<name>` (warnings as errors,
  focused `ctest` subset, format check on changed files) and report the diff, the tests run, and
  open questions. They do not push, merge, touch `main`, or commit; the shared git index and every
  push stay with the Lead.
- **The Lead owns the gate.** The Lead reviews the diff, re-runs the matching verification at the
  exact HEAD, commits with explicit pathspecs, pushes the branch, opens the PR (naming the
  subagent's contribution), and merges only when the PR is green.
- **Re-slice instead of stretching scope.** A subagent that finds the item larger than agreed
  returns the analysis and stops editing; the Lead splits it again. Subagent output is input to
  the Lead's decision, never a merge on trust.
- Use `subagent` for isolated parallel lanes; use `spawn_teammate` only for a long-lived role that
  needs its own task board row (teammates are append-only and capped). Keep cross-cutting
  refactors, AGENTS.md edits, and shared index files on the Lead.

### Commit batching

One coherent change is one commit: a subsystem slice plus its tests, docs, and index/status
updates belongs together, not in a trail of tiny commits. Do not split a single logical change
just to make diffs smaller, and do not pad a batch to reach a size target. Split only when each
part is independently reviewable or independently revertible. Commits are local; what reaches CI is
a landing (see [Landing size and CI load](#landing-size-and-ci-load)). Before every commit, run the
verification that matches the change (format, build with warnings as errors, `ctest`, and the
relative-link/index checks for docs); a docs-only batch still needs the format and link checks.

### Context compaction

Context compaction is automatic: the harness compacts the session in place at its configured
threshold and the run continues. Do not track a manual context-percentage checkpoint, stop to run
`/compact`, or ask the owner to compact; finish the current coherent batch, keep the tree green,
and continue. Re-read Current focus and the task board after a compaction before picking work.
This is a tooling workflow note, not a Tessera runtime feature or configuration key.

## Big Steps (group work, don't micro-step)

The **V1 milestone is the unit of scheduling**; Big Steps A-F keep their names and sit under the
milestone that finishes them. Milestone descriptions on GitHub hold the acceptance criteria. Each
open item is one shippable slice (behavior, tests, docs), acceptance after the dash. `[x]` means on
`main` with an evidence path; a partly done item stays `[ ]` and says what is left.

### Big Step A — Foundation & risk proof *(complete)*
- [x] CMake on Windows/Linux; Vulkan 1.3 offscreen PNG on lavapipe CI; merger prototype; CI.

### V1-M1 Core runtime ([milestone 1](https://github.com/thnak/Tessera/milestone/1)) — Big Step B
Acceptance: the static dashboard idles with zero presents; goldens green in CI on Windows and Linux.

- [x] **B1:** RHI, null + Vulkan backends, batched 2D, scene graph/events —
      [`docs/images/batched2d.png`](docs/images/batched2d.png).
- [x] **B2:** Yoga Flexbox adapter — `src/ui/layout/layout_engine.cpp`, `third_party/yoga`.
- [x] **B2:** Input: normalized events, focus, pointer capture, pseudo-states —
      `src/ui/scene/input_dispatch.cpp`, `tests/unit/input_hover_tests.cpp`.
- [x] **B2:** Fixed-step loop: accumulator, HAL clock, pacing, deterministic driver —
      `src/runtime/{fixed_step,system_clock,runtime_loop,frame_pacer}.cpp`.
- [x] **B2:** Async: cancellation, executors, completion queue, `Task<T>`, I/O executor —
      `src/runtime/{async,worker_executor,task,io_executor}.cpp`.
- [x] **B2:** Pixel goldens via lavapipe/CPU — `batched2d_golden`, `batched2d_cpu_golden` in
      `tests/gpu/CMakeLists.txt`.
- [x] **B2:** Stylesheet: `var()` substitution with inheritance, fallback and cycle detection —
      `src/ui/style/var_resolve.cpp`, `tests/unit/cascade_tests.cpp`, `tests/unit/style_var_tests.cpp`.
- [x] **B2:** Stylesheet: `text-transform` (`uppercase`/`lowercase`/`capitalize`) — text paint
      test — `src/ui/text/text_transform.cpp`, `tests/unit/text_transform_tests.cpp`.
- [x] **B2:** Stylesheet: `word-spacing` — `src/ui/text/text_wrap.cpp`,
      `tests/unit/word_spacing_tests.cpp`.
- [x] **B2:** Text stack done except ICU in CI: add the `icu-text` job
      (`TESSERA_ICU_REDUCED_DATA=ON`, filtered text ctest) — `.github/workflows/linux.yml`,
      `docs/design/icu-data.md`.
- [x] **B2:** Dirty rendering done except per-node paint caches — clean subtrees are not
      re-recorded; damage equivalence still passes —
      `src/ui/paint/paint_cache.cpp`, `src/ui/frame/frame_pipeline.cpp`,
      `tests/unit/paint_cache_tests.cpp`, `tests/unit/frame_paint_cache_tests.cpp`.
- [x] **B2:** Pacing done except dynamic refresh/VRR — pacer follows a scripted rate change —
      `src/runtime/frame_pacer.cpp`, `tests/unit/frame_pacer_tests.cpp`.
- [x] **B2:** Power: adaptive quality and occluded/background pause — occluded window presents
      nothing (overlaps ADR-050 AF3) — pause landed (`src/app/window_lifecycle.cpp`,
      `tests/gpu/app_window_occluded_tests.cpp`); `pauseWhenOccluded` is wired through
      `WindowOptions::runtime` (`src/app/window_config.cpp`); the adaptive-quality controller
      `src/runtime/adaptive_quality.cpp` reports a clamped render-scale decision with hysteresis
      (`tests/unit/adaptive_quality_tests.cpp`) and the host consumes it
      (`src/ui/scale/render_target_extent.cpp`, `tests/gpu/app_window_render_scale_tests.cpp`):
      a downgrade renders into a smaller target and blits it up, while scale 1.0 keeps the
      direct path and every golden unchanged. Step 3 landed too: a downgrade lowers the present
      cap to the next refresh divisor (`src/runtime/present_cap.cpp`,
      `tests/gpu/app_window_present_cap_tests.cpp`), and its divisor base is a real HAL query
      (`hal::Window::display_refresh_hz()`, `src/hal/window/refresh_rate.cpp`: Win32 monitor
      mode, X11 RandR, unknown elsewhere). Steps 1 and 4 are the ladder's two ends
      ([#147](https://github.com/thnak/Tessera/issues/147), decisions in `performance.md` §6.1):
      position 1 caps blurred box-shadow layers (`PaintContext::reduce_effects`), the last
      position freezes paint-only animations (`AnimationRuntime::set_suspend_non_essential`). The frame-time
      signal comes from GPU timestamps when the device reports usable ones and from the CPU
      otherwise (`include/tessera/rendering/rhi/timestamp.hpp`,
      `tests/unit/gpu_frame_time_tests.cpp`).
- [x] **B2:** Multi-frame sync: timeline semaphores (two frames in flight already work) — smoke
      test validation-clean — `src/rendering/backend/vulkan/src/vulkan_swapchain_present.cpp`,
      `tests/gpu/vulkan_timeline_frame_tests.cpp`.
- [x] **B2:** Interpolation alpha wake threshold + `animationInterpolation` config key —
      `animationInterpolation` is wired through `WindowOptions::runtime`
      (`src/app/window_config.cpp`); the threshold is decided (duplicate-present suppression, never a
      wake source) and implemented for the interpolated box as `interpolation_epsilon_px` (runtime key
      `interpolationEpsilonPx`); suppressing the dirty-driven present itself is tracked in #200, pending a measurement
      ([#148](https://github.com/thnak/Tessera/issues/148)).
- [x] **B2:** Call the shader manifest validator before `glslc` — bad manifest fails the build —
      `tools/shaders/manifest.json`, `tools/shaders/manifest_check.cpp`,
      `src/rendering/backend/vulkan/CMakeLists.txt`,
      `tests/unit/manifest/manifest_build_step_tests.cpp`.
- [x] **B2:** Harness goldens: baselines in `tests/golden/baseline/harness/`, run on lavapipe CI —
      `tests/gpu/harness_frame_capture_tests.cpp`, `tests/golden/baseline/harness/fixture_golden.png`.
- [x] **B2:** Wider CPU goldens (borders, clips, gradients) — bit-exact on every platform —
      `tests/gpu/cpu_wide_golden_tests.cpp`, `tests/golden/baseline/cpu_wide/`.
- [x] **B2:** Host-owned `CaretBlink` in `WindowHost` — toggles under `ManualClock`; idle when
      unfocused — `src/app/window_lifecycle.cpp`, `tests/gpu/app_window_caret_tests.cpp`.
- [x] **B2:** `scrollIntoView` reveals an unrealized virtual item via `VirtualRange` — harness
      script — `src/harness/manifest_virtual.cpp`,
      `include/tessera/automation/virtual_collection.hpp`,
      `tests/unit/harness/scroll_into_view_tests.cpp`.
- [x] **B2:** Compile the POSIX `poll` path of `LineTransport::wait_readable` — real POSIX stdio
      test — `src/harness/stdio_transport.cpp`, `tests/unit/harness/stdio_transport_posix_tests.cpp`.
- [x] #155: **B3:** Wayland window + Vulkan surface (Win32/X11 incl. swapchain retirement and device-loss
      recovery done) — smoke test. A Linux backend selector (`src/platform/linux/window_linux.cpp`,
      pinned by `TESSERA_WINDOW_BACKEND`) picks X11 or Wayland per session, so a box with only one
      dev package still builds; either backend is compiled only when found. Wayland lives in
      `src/platform/linux/window_wayland{,_dispatch,_shell}.{cpp,hpp}` and
      `window_wayland_internal.hpp` (xdg-shell handshake with ping/pong, never draw before
      `ack_configure`, `wl_display` pump at the tick boundary); the code comes from
      `cmake/TesseraWayland.cmake`, which runs `wayland-scanner client-header`/`private-code` at
      configure time as a build artifact (never committed). The Vulkan half is
      `src/rendering/backend/vulkan/src/vulkan_present_platform.cpp` (`PresentPlatform` selection)
      plus the `VK_KHR_wayland_surface` path in `vulkan_swapchain.cpp`, and the RHI carries it as
      `rhi::SurfacePlatform` (`include/tessera/rendering/rhi/{types,swapchain,version}.hpp`, RHI 1.4).
      Evidence: `window_wayland` and `vulkan_swapchain_wayland` (`tests/unit/window_wayland_tests.cpp`,
      `tests/gpu/vulkan/vulkan_swapchain_wayland_tests.cpp`) pass against a real weston 14 headless
      compositor (`window_wayland: ok (320x200 on wayland-tessera)`,
      `vulkan_swapchain_wayland: ok (320x200, presents=1, alpha=premultiplied)`); the X11 suite is
      unchanged on the same build (`window_x11`, `window_x11_input`, `window_x11_real_input`,
      `window_metrics`, `rhi_null`), and X11-only and no-display configures both build. Windows and
      Linux both build warnings-as-errors. Not exercised: a real (non-headless) compositor, fractional
      scaling, and a Wayland session under XWayland (the X11 tests now pin the backend so they cannot
      silently pick Wayland).
- [x] **B3:** Linux input: XInput2 scroll/touch/pen, IME pre-edit (#55) — synthetic pump
      tests — `src/platform/linux/x11_xi2_translation.cpp` (valuator scroll, touch, pen),
      `src/platform/linux/x11_ime_composition.cpp` (pre-edit state machine),
      `tests/unit/x11_input_translation_xi2_tests.cpp`; the pump selects XI2 when present and
      falls back to the unchanged core path. Touch/pen/XIM were exercised only at the
      translation layer and on Xvfb (no touch/pen hardware, no IM server), and nothing paints
      the pre-edit yet because there is no text editor.
- [ ] #156: **B3:** OS events (ADR-023, accepted): first slice is the normalized `OsEvent`/`WindowMetrics`/`FoldInfo`
      types, a synthetic event injector, and display-capability events on Windows and X11 (DPI, scale,
      refresh, work area) — synthetic display scripts pass; fold/posture/hinge backends are post-V1
      (Android).
- [x] #227: Effective opacity multiplies down the subtree (children fade with their parent) —
      `src/ui/paint/paint_pass.cpp` (ancestor product folded into every primitive), paint-cache key
      (`PaintCacheSignature::opacity_bits`), `Node::mark_subtree_dirty` on an animated opacity,
      `docs/design/paint-pass.md` §14.1 (non-isolated rule + deferred isolation seam), test
      `paint_opacity` (0.5 × 0.5 = 0.25, hit-testable `opacity: 0`, animation-driven subtree repaint).
- [x] #242: Renderer seam: render-pass break, `copy_target_region`, pooled offscreen target (glass) —
      `include/tessera/rendering/rhi/device.hpp` (RHI 1.1 op + contract),
      `src/rendering/gpu/{batched2d_seam,offscreen_target_pool}.cpp`,
      `src/rendering/backend/vulkan/src/vulkan_command_list_copy.cpp`,
      `docs/design/gpu/offscreen-layers.md`, `docs/design/paint-pass.md` §14, tests `renderer_seam`,
      `renderer_seam_gpu`, `rhi_null` (300-frame bounded pool, byte-identical no-break commands).
- [x] #244: `backdrop-filter` glass, two-pass Gaussian, damage rules, GPU composite (seam from #242,
      sampled target from #254) — `src/rendering/gpu/batched2d_glass.cpp`
      (`Batched2DRenderer::draw_backdrop`: break -> region copy -> horizontal blur -> resume ->
      vertical blur + composite, bounded by `glassMaxBlurTargets`), `src/ui/paint/paint_sinks.cpp`
      (the real frame path; `GlassPaintStats` counts composites vs the bounded fallback),
      `src/app/window.cpp` + `window_recovery.cpp` + `frame_capture.cpp` (glass pipelines and limits;
      a glass frame renders into an offscreen target because a swapchain image is not a copy source),
      the `opacity` field in `tools/shaders/manifest.json` / `glass_blur.frag` / `glass_blur.wgsl`,
      docs `docs/specs/glass-v1.md` §3/§5/§8/§9 (with the `glass_ms` semantics pinned in
      `docs/specs/runtime-config-v1.md` §3), `docs/design/gpu/offscreen-layers.md` §5.1 and `docs/design/gpu/rhi.md` §5.1,
      `docs/design/paint-pass.md` §14, and the benchmark scenario in `tests/golden/perf/cpu_benchmark.txt`
      (`max_glass_paint_ms`, `glass_backdrops`); tests `glass_paint_null` (real sink, command order and
      every bounded fallback), `glass_paint_gpu` (real paint path pixels: analytic composite, kernel
      scan, half resolution, rounded clip, opacity, `blur(0)`, validation-clean), plus the existing
      `paint_glass`, `glass_damage` (static glass = zero frames), `glass_blur`, `glass_style`,
      `glass_config` and the unchanged image goldens.
- [x] #241: `tessera_benchmark`, a GPU-free 12k-node CPU benchmark with a committed budget —
      `tests/benchmarks/cpu/` (CTest `cpu_benchmark`), `tests/golden/perf/cpu_benchmark.txt`,
      measured table and the paint trigger in `docs/design/performance.md` §9.1.
- [x] #230: Logical properties (`*-inline-*`, `text-align: start/end`) and the direction contract —
      `src/ui/style/logical_properties.cpp` (registry + shorthands + diagnostics),
      `src/ui/layout/layout_style.cpp` (Yoga Start/End edges), `layout_signature.cpp`,
      `src/ui/paint/paint_{properties,text,pass}.cpp` (mirrored borders and `text-align`),
      `docs/specs/stylesheet-v1.md` §11.1, `docs/specs/i18n-v1.md` §4/§7/§8.1, tests
      `logical_properties`, `logical_layout` (same stylesheet mirrors in LTR/RTL).
- [x] #235: The `::part()` pseudo-element in the stylesheet engine —
      `src/ui/style/{selector,selector_match,selector_parts,selector_support,selector_index}.cpp`,
      `component_parts.cpp` (`TESSERA_CSS_UNDECLARED_PART` for library components),
      `Node::set_part/set_component_name` (lazily allocated metadata, `sizeof(Node)` unchanged),
      `docs/specs/stylesheet-v1.md` §6.1–6.4/7.2/11/12/15/16, `compiled-stylesheet-v1.md` §2–5,
      tests `part_pseudo`, `part_attribute` (parse, specificity, cross-boundary matching, cascade,
      `@supports selector()`, blob round trip).
- [x] #247: `pointer-events`, `user-select` and `aspect-ratio` registered and implemented (#214) —
      `src/ui/style/{property_registry,interaction_values,interaction_style,supports,cascade}.cpp`,
      `src/ui/scene/node.cpp` (hit-test skip + selection gate), `src/ui/layout/layout_style.cpp`
      (explicit width+height win), `docs/specs/stylesheet-v1.md` §11, tests `interaction_style`,
      `property_registry`, `layout` height-wins. Covers #215 and #216; #217 lands per its fallback
      clause (`user-select: all` stays `UnsupportedValue`, static-text selection is a follow-up).
- [x] #248: `scroll-behavior`, `overscroll-behavior`, `overscroll-effect` and `scrollbar-gutter` wired to
      the scroll controller (#214) — `src/ui/style/{property_registry,property_value_validation,supports}.cpp`,
      `src/ui/scene/{scroll,scroll_animation}.cpp` (chain/smooth/gutter), `style::sync_scroll_style`,
      `src/ui/layout/{layout_style,layout_signature}.cpp` (gutter inset + dirty rule),
      `docs/specs/{stylesheet-v1,scroll-and-gestures-v1,animation-v1}.md`, test `scroll_properties`.
- [x] #249: CSS Grid subset with a C++ `GridEngine`; Yoga has no grid, taffy only through an ADR (#214) —
      `src/ui/layout/{grid_engine.hpp,grid_layout.cpp,grid_placement.cpp,grid_tracks.cpp}` (engine internal
      to the layout package), `src/ui/style/grid_values.{hpp,cpp}` (one grammar for the validator,
      `@supports` and the engine), registry + `layout_signature` + scheduler dirty, `docs/specs/grid-v1.md`,
      tests `grid_layout` (box corpus, determinism, 1,000 items in 15 ms), `grid_signature`, `grid_style`.
- [x] #250: Test that the stylesheet registry matches the V1 matrix of `stylesheet-v1.md`; it uses a
      known-gaps allow-list that #247 to #249 empty — `docs/specs/stylesheet-v1.properties.txt`
      (source of truth, one `<kind> <name> <§11 row>` per line),
      `tests/unit/property_registry_matrix_tests.cpp` (walks both directions, names the property and
      its row, allow-lists are `kKnownGaps` — the four Grid names while #249 is open — and
      `kEngineInternal`), `src/ui/style/property_registry_extra.cpp` (the `border` shorthand and the
      animation/transition names, which made `@supports (transition: none)`/`(animation: none)` true).
- [x] #213: Every registered property has a real effect or a recorded status —
      `docs/specs/stylesheet-property-status-v1.md` (per-longhand table, linked from
      `stylesheet-v1.md` §11), new consumers for `order` (`src/ui/layout/layout_engine.cpp`) and
      `visibility` (`Node::set_style_hidden`/`rendered()` gating paint, hit test, a11y, focus,
      damage), `PropertyRegistry::longhand_names()`, tests `property_effects` and `property_status`
      (the latter fails when the registry and the table disagree).


### V1-M2 Compiler and animation ([milestone 2](https://github.com/thnak/Tessera/milestone/2)) — Big Step C
Acceptance: a `useState`/`useEffect`/`useRef` screen regenerates without losing developer C++.

- [x] JSX/TSX front end + Tier-1 rules — `src/compiler/{analyzer,rules}/`.
- [x] `bind::` extractor + IR — `src/compiler/{ir,lowering}/`.
- [x] C++ codegen + incremental merger — `src/compiler/codegen/`, `tools/codegen/`.
- [x] Acceptance (legacy merger) — `samples/compiler-demo/Demo.tsx`,
      `tests/golden/codegen/demo/Demo.cpp`.
- [x] Animated values (tween, spring, `onComplete`, reduced motion live on Windows), CSS end events,
      `SpringRuntime` in the frame pipeline — `src/ui/animation/`, commits `a76165f`, `a075170`.
- [ ] #157 (slice 1 landed), #209-#211, #218, #219 (one canonical codegen path: scaffold, consumers, tests, delete legacy, docs): ADR-040: emit `gen::<Component>Base<Self>` + developer class — `compiler-demo` regenerates
      with its developer class untouched.
- [x] #81/ADR-048: embed the compiled stylesheet blob as a `constexpr` array (serializer/loader
      landed) — no `TESSERA_CG_STYLESHEET_NOT_EMBEDDED` for embedded sheets —
      `src/compiler/codegen/src/emit_stylesheet_embed.cpp`,
      `tests/unit/codegen_stylesheet_embed_tests.cpp`.
- [x] #81/ADR-048: `AppHost` registers embedded sheets in import-graph order, library CSS in
      `@layer tessera-ui` — cascade-order test — `src/app/host.cpp`,
      `src/ui/style/compiled_stylesheet.cpp`, `tests/unit/app_host_stylesheet_tests.cpp`,
      `tests/unit/compiled_stylesheet_tests.cpp`.
- [x] #81/ADR-048: embed the theme asset through the same blob — loads without parsing —
      `src/ui/style/theme_asset_blob.cpp`, `src/compiler/codegen/src/emit_theme_embed.cpp`,
      `src/app/host_theme.cpp`, `tests/unit/{theme_asset_tests.cpp,app_host_theme_tests.cpp}`.
- [x] #201/ADR-048: startup benchmark comparing the embedded blob with runtime parse — counters,
      not wall clock — `tests/benchmarks/stylesheet/` (`benchmark.css`, `benchmark_paths.cpp`,
      `benchmark_counters.cpp`), baseline `tests/golden/perf/stylesheet_benchmark.json`, opt-in
      ctest `stylesheet_startup_benchmark` behind `-DTESSERA_BENCHMARK_STYLESHEET=ON`; result in
      `docs/specs/compiled-stylesheet-v1.md` §1 (4488 fewer prepare allocations and 3.78 MB less
      churn for the identical cascade, at a 63 % larger payload).
- [x] #271: transition declarations and the computed-style animation bridge run in the frame
      pipeline — `src/ui/animation/style_bridge.{hpp,cpp}` (`StyleBridge::sync` diffs each rendered
      node's computed style and calls `start_style_transitions`), `parse_style_transitions` in
      `animation_style.cpp`, the `FramePipeline::sync_style` hook, `AppHost` installing the sheet's
      `@keyframes` with `set_style_keyframes`, decision record in
      `docs/design/animation-runtime.md` §9, tests `transition_wiring` and the app-host
      `css_animation_starts_from_the_installed_keyframes` case. The five transition rows in
      `stylesheet-property-status-v1.md` §8 are `applied`.
- [ ] #158: ADR-051 accepted; #70 leftovers, one slice each: cross-file props aliases; object-literal prop defaults; stable
      component identity (inserting a component keeps other ids and goldens) — stable identity
      landed (`src/compiler/ids/src/assign_identity.cpp`, `tests/unit/ir_ids_stability_tests.cpp`)
      and cross-file props aliases landed
      (`src/compiler/lowering/src/type_alias_table.cpp`, `tests/unit/lowering_props_alias_tests.cpp`);
      still to do: object-literal defaults (blocked on an IR record type).
- [x] Scene-side `EventType::Scroll` for virtual collections — dispatch test —
      `include/tessera/ui/scene/event.hpp`, `tests/unit/scene_scroll_event_tests.cpp`.
- [x] Dynamic `orientation`/`columns`/`overscan` on virtual lists/grids — codegen runtime test —
      `src/compiler/codegen/src/emit_virtual_geometry.cpp`,
      `tests/unit/codegen_dynamic_virtual_tests.cpp`, `tests/golden/codegen/dynamic/`.
- [x] Codegen emission of vector payloads (`PaintKind::Vector`) — generated golden —
      `src/compiler/codegen/src/emit_vector.cpp`,
      `tests/unit/codegen_vector_payload_tests.cpp`,
      `tests/gpu/codegen_vector_paint_golden_tests.cpp`,
      `tests/golden/baseline/codegen/vector_payload_cpu.png`.
- [x] #226: Focusable `View`: `tabIndex`, `autoFocus`, `onFocus`/`onBlur`/`onKeyDown`, `ref.focus()`. Evidence: `focusable_view` (`tests/unit/focusable_view_tests.cpp`), `schema_golden`; spec [common-and-events.md](docs/specs/jsx-intrinsics-v1/common-and-events.md) section 4.
- [x] #229: Inline spans in `Text` (per-run class, `onClick`, `role="link"`) —
      `src/compiler/lowering/node_children.cpp` (span children + synthetic runs, `TESSERA_JSX_SPAN_*`),
      `src/compiler/ir/src/validate_node.cpp` (`TESSERA_IR_SPAN`), `src/compiler/codegen/src/emit_node.cpp`
      (`PaintData` text + `PaintSpan` byte ranges), `src/ui/paint/paint_text_spans.cpp` (per-run weight/italic/
      colour, wrapping across spans), span run rects for hit testing, a11y link children;
      `alias_style_defaults` for b/strong/i/em/code and `a` → `role="link"`; specs
      `elements-basic.md` §2.1, `html-aliases.md`, `ir-v1.md` §3.6/§9/§10; tests
      `lowering_inline_span`, `ir_inline_span`, `codegen_span`, `paint_text_span`, `a11y_span`.
- [x] #231: User component instances inside list templates and conditionals, and list-typed props —
      design in `docs/design/compiler-node-state.md` (one `gen::<Callee>` per instance; a conditional
      instance keeps its object in the caller and only its host node moves into the branch slot, a
      keyed list item owns one instance inside its keyed record), codegen
      `src/compiler/codegen/src/emit_instances*.cpp`, goldens `tests/golden/codegen/instances/`,
      tests `codegen_instances` + `codegen_instances_runtime` (state survives reorder, key removal
      unmounts, per-item presence, list-typed prop); open decision on conditional-instance
      destroy-vs-keep-alive recorded in #283.


### V1-M3 Backends and shader chain ([milestone 3](https://github.com/thnak/Tessera/milestone/3))
ADR-049 is Accepted and the shader chain is on `main`; WebGPU is Tier 1: its CI lane (`wasm-build` + `wgsl-validation`) is green on GitHub (promoted 2026-11-13).

- [x] WebGPU first slice (mapping tables, WGSL registry, unbuilt skeleton) —
      `src/rendering/backend/webgpu/`, `tests/unit/webgpu_mapping_tests.cpp`.
- [ ] #159: `shader_id`, portable shader branch, opt-in cross-compile — PR #84, after ADR-049.
- [x] #160: RHI `version.hpp` + `Device::capabilities()` + docs-gate check on RHI header changes —
      `include/tessera/rendering/rhi/version.hpp`, `RhiCapabilities` reported by null/Vulkan/WebGPU
      (`tests/gpu/rhi/rhi_null_tests.cpp`, `rhi_buffer_bounds_tests.cpp`), gate in
      `scripts/check_docs.py` (`scripts/tests/test_check_docs.py`).
- [x] #161: Backend-selectable GPU harness, `tolerance.json`, conformance summary — null/Vulkan pass —
      `tests/gpu/conformance/` (`gpu_conformance_null`, `gpu_conformance_vulkan`,
      `gpu_conformance_summary`), `tests/golden/tolerance.json` resolved by
      `scripts/golden_tolerances.py` in `tests/gpu/CMakeLists.txt`.
- [x] #162: `docs/guides/writing-a-backend.md`, tiers table, backend build tools in dependencies.md —
      [`docs/guides/writing-a-backend.md`](docs/guides/writing-a-backend.md) (tiers, RHI members,
      version/`capabilities()`, shaders, build wiring, suite, checklist),
      [`docs/specs/dependencies.md`](docs/specs/dependencies.md) "Backend build tools".
- [x] #163: Emscripten CI lane + WGSL validation — WebGPU promoted to Tier 1 —
      `.github/workflows/webgpu.yml`, `scripts/ci/validate_wgsl.py`, ADR-049.
- [ ] #245: Cross-backend image parity: a fixed scene set on every available backend against the
      lavapipe reference, SSIM and deltaE with edge-rounding vs structural classes, tolerances per
      backend pair in `tolerance.json`.

### V1-M4 Design-time preview and playground ([milestone 4](https://github.com/thnak/Tessera/milestone/4)) — Big Step D
Acceptance: a screen previews in a browser and matches native output within the golden tolerance.
Stages P0-P7 are planned in `docs/design/reference-equantic-ui.md`.

- [ ] #164: P0: Emscripten preset, compiler in Wasm — IR byte-identical to native for
      `samples/parity-demo/Counter.tsx`.
- [ ] #165: P1: diagnostics-only page on GitHub Pages — diagnostics match native.
- [ ] #166: P2: evaluator (ADR-037) + scene + layout on the null RHI — snapshot equals native harness.
- [ ] #167: P3: WebGPU canvas preview — within tolerance of the lavapipe golden (needs M3).
- [ ] #168-#171: P4-P7: browser input, share links, CI budgets, picker/style patches, hosted studio, hot
      reload (#78), one stage per PR.
- [ ] #172: `tessera-mcp` stdio adapter over the harness session, compile-only first.

### V1-M5 Application framework ([milestone 5](https://github.com/thnak/Tessera/milestone/5))
ADR-050 is Accepted (roadmap AF1-AF8); start with AF1.

- [ ] #173: AF1: `ServiceContext` + `Service` state machine, ordered async start/stop, deadlines — new
      `tessera_appfw_core`, tests under `ManualClock`/`ManualExecutor`.
- [ ] #174: AF2: `TaskScope` + `RuntimeLoop` timer wake source — timers in order; idle parks.
- [ ] #175: AF3: `Application` lifecycle; `WindowHost` forwards focus/minimize/occlusion.
- [ ] #176: AF4: `Observable<T>` over `UpdateScheduler` — one flush per tick, no allocation on `set`.
- [ ] #177: AF5: `useService<T>()` codegen (after ADR-040) — undeclared service is a compile error.
- [ ] #178: AF6: `NativeSurface` refs + `SurfaceRenderer` — lavapipe golden of a composited surface.
- [ ] #179: AF7: `storage` module, atomic writes on `IoExecutor` — corruption fallback tested.
- [ ] #180, #181: Multi-window, then tray apps, each its own slice.
- [ ] #228: One `Application` loop and one `rhi::Device` for several windows (before AF6).
- [x] #234: Library stage 0: bare package imports of `tessera-ui` —
      `packages/tessera-ui/` (Field, Form, `tessera-package.json`), `tools/codegen/package_manifest.cpp`
      (version check), `src/compiler/lowering/src/module_lowering.cpp` (bundled `tessera-ui` imports,
      `from "tessera"` rejection), `docs/specs/package-manifest-v1.md`; tests `forms_lowering`,
      `codegen_cli`, `library_tsx_parity`.
- [x] #236: Library stage 0: behavior primitives (evidence: `packages/tessera-ui/primitives/`, `tests/unit/primitives_tests.cpp`, `tests/unit/focus_scope_tests.cpp`, `tests/unit/guarded_handler_tests.cpp`).
- [x] #237: Library stage 0: Instrument tokens, light/dark/high-contrast themes, theme versioning and
      pinning, WCAG contrast tests (evidence: `packages/tessera-ui/themes/instrument.theme.json`,
      `tests/unit/{instrument_theme_tests.cpp,theme_version_tests.cpp}`). The bundled font pack and
      swatch goldens moved to #280.
- [ ] #280: Library stage 0: IBM Plex bundled font pack, theme pin of the pack, token swatch goldens.
- [ ] #286: Callback props on user components. Step 1 (forwarding `onClick={onPress}`) done: `src/compiler/lowering/src/callback_forward.cpp`, IR `callProp`, codegen `emit_call_prop`, tests `lowering_callback_forward`, `codegen_callback_forward_runtime`, spec `jsx-subset-v1.md` section 9.5. Step 2 (calling a callback from a handler body, `callback_forward.cpp` + `handler_actions.cpp` `check_callback_calls`) done in the stacked PR. Step 3 (performance gate: zero allocations per forwarded event, depth bounded by IR cycle check, `dispatch_event_to` no longer allocates its path) done in its own PR.
- [x] #238: Library stage 0: `tessera icons add` (Iconify import tool with the license gate) —
      `tools/icons/` (`tessera_icons_core` + the CLI), license gate per ADR-047 item 8 with a unit
      test per class (`icons_license`, `icons_gate`, `icons_cli`), pinned fixtures
      `tests/fixtures/icons/`, byte-exact golden `tests/golden/icons/{basic,monochrome}` with
      determinism + tree-shaking (`icons_import`), `THIRD_PARTY_NOTICES.md` icon-set policy,
      `docs/design/component-library.md` §3.1 status.
- [x] #239: Library stage 0 exit: a styled Button end to end and the gallery skeleton (native lane; the web lane is #290). Evidence: `packages/tessera-ui/Button.tsx`, `samples/gallery`, `tests/golden/gallery`, ctest `library_button_runtime` and `gallery_golden_*`.
- [ ] #240: Library stage 1 tracking (ready; #223 `SafeArea` is its first item).
- [x] #223: `SafeArea` (safe-area-v1): inset arithmetic, host-resolved padding or margin with per-edge easing, the library component and gallery page. AppBar/Sheet/Toast defaults, `TwoPane` and the focused-field scroll are follow-ups. Evidence: `packages/tessera-ui/SafeArea.tsx`, `include/tessera/hal/safe_area.hpp`, `src/app/safe_area_state.cpp`, ctest `safe_area`, `app_safe_area`, `library_safe_area_*`, `gallery_golden_safe_area_*`. Stages 2 to 6
      (including `LiveCapsule`, #224) follow V1.
- [x] #302: Overlay layers (overlays-and-layers-v1): the layer core (`OverlayHost`, four layers, handles, frame-boundary open/close, modal scrim, focus trap, `aria-modal` inertness) and slice 2 (pure placement with flip/shift/RTL and a window anchor, per-frame anchor tracking inside the safe viewport, Escape/back/outside/blur dismissal, `InputPump` filter). Evidence: `include/tessera/ui/overlay/overlay.hpp`, `placement.hpp`, `src/ui/scene/overlay_*.cpp`, ctest `overlay_host`, `overlay_placement`, `overlay_dismiss`. The `tessera-ui` surface and non-modal focus are follow-ups; this unblocks #224.


### V1-M6 Data-heavy UI ([milestone 6](https://github.com/thnak/Tessera/milestone/6)) — Big Step E
Acceptance: a streaming dashboard at target frame rate with deterministic goldens.

- [x] Fixed-timestep clock + time scale — `include/tessera/runtime/fixed_step.hpp`.
- [x] #182: Zero-allocation ring buffers for chart series — allocation-counting test —
      `include/tessera/ui/chart/chart_series.hpp` (`ChartSample{double,float}` + fixed-capacity
      `ChartSeriesRing`: overwrite-oldest, logical oldest→newest, two-span renderer view, strictly
      increasing timestamps, `noexcept` and allocation-free after construction),
      `docs/specs/chart-series-v1.md` (capacity/overwrite/wrap/iteration/threading contract),
      tests `chart_series_ring` (`chart_series_ring_tests.cpp` semantics +
      `chart_series_ring_allocation.cpp` replaced-global-`operator new` zero-allocation proof across
      five wrap-arounds).
- [ ] #183: Retained/immediate hybrid chart nodes with damage rects; frame-budget metrics in the overlay.
- [ ] #184: Streaming dashboard sample under sustained load with deterministic goldens.
- [x] #233: `DataStore::set/update` push entry for app-fed data —
      `include/tessera/runtime/data_store.hpp`, `src/runtime/data_store_core.cpp` (per-key version +
      fetch epoch, CompletionQueue tick notification, bounded LRU), `docs/specs/async-data-v1.md`
      §8/§8.1 + G-21, test `data_store` (notification/coalescing, last-write-wins, push over an
      in-flight fetch, eviction).


### V1-M7 Platform breadth and accessibility ([milestone 7](https://github.com/thnak/Tessera/milestone/7)) — Big Step F
- [x] #185: Install `make_window_root_provider` from `WindowHost` (UIA host provider exists) — Narrator/
      NVDA smoke checklist — `src/app/accessibility_host.{hpp,cpp}` + `src/app/window_accessibility.cpp`
      (the controller `WindowHost` owns: forwards `hal::Window::accessibility_host_handle()` to the
      platform seam, installs `make_window_root_provider(UiaHost&)`, uninstalls it on stop and before
      the window dies), `docs/specs/accessibility-v1.md` §12.3 + the manual checklist in §12.3.1,
      test `app_accessibility_host`.
- [x] #186: Wire `AtspiSession` into the Linux host; failures turn accessibility off with a diagnostic —
      `src/app/accessibility_host_atspi.cpp` (`AtspiBridge` + `AtspiSession`, the session's own worker
      thread pumps, a terminal phase becomes one `TESSERA_A11Y_UNSUPPORTED:` line),
      `docs/specs/accessibility-v1.md` §12.3/§12.3.2, tests `app_accessibility_host`
      (synthetic loopback peer: handshake, auth rejection, peer close).
- [ ] #187: Reduced motion and dark mode queries on Linux.
- [x] #243: Translucent window and OS backdrop (Mica, Acrylic, compositor blur) with fallback —
      `include/tessera/hal/window_backdrop.hpp` (resolver table), `src/platform/windows/window_backdrop.cpp`
      (`DWMWA_SYSTEMBACKDROP_TYPE`, DWM frame extension), `src/platform/linux/x11_backdrop.cpp`
      (ARGB visual + `_KDE_NET_WM_BLUR_BEHIND_REGION`), `include/tessera/rendering/rhi/swapchain.hpp`
      (RHI 1.2 composite alpha), `tests/unit/harness/backdrop_tests.cpp`,
      `docs/specs/window-chrome-v1.md` §11.1 manual checklist. Wayland `ext-background-effect-v1`
      stays gated on #155.
- [x] #222: `CutoutInfo` in `WindowMetrics` with a fake for tests (the Android fill is post-V1). Evidence: `tests/unit/harness/cutout_tests.cpp`, `include/tessera/hal/cutout.hpp`.
- [x] #232: Window full-screen toggle on Windows and Linux —
      `include/tessera/hal/{window,fullscreen,caption}.hpp` (`set_fullscreen`/`supports_fullscreen`,
      the platform-neutral `FullscreenState` bookkeeping, and the `CaptionRegion` contract),
      `src/platform/windows/window_fullscreen_win.*` (borderless window over the current monitor,
      `WINDOWPLACEMENT` saved/restored) and `src/platform/windows/window_caption_win.*`
      (`WM_NCCALCSIZE`/`WM_NCHITTEST`/`WM_NCLBUTTONDOWN`), `src/platform/linux/window_fullscreen_x11.*`
      (EWMH `_NET_WM_STATE_FULLSCREEN`, mode read back from `_NET_WM_STATE`), app entry point
      `include/tessera/app/window.hpp` (`WindowHost::set_fullscreen`/`fullscreen`/
      `fullscreen_supported`), `docs/specs/window-chrome-v1.md` §8.1/§8.4, tests `window_fullscreen`,
      `window_fullscreen_fake`, `window_win32`. Wayland stays gated on #155.
- [x] #191: Platform cursor shape on Windows and Linux — the CSS `cursor` keyword set is a shared
      grammar (`include/tessera/ui/style/cursor_value.hpp`, `src/ui/style/cursor_value.cpp`) used by
      the declaration validator, `@supports` and the computed-style sync; `Node::cursor` carries the
      resolved kind, `src/app/window_cursor.*` maps it onto `hal::CursorShape` and requests it only on
      a change, and `include/tessera/hal/cursor.hpp` + `WindowMetrics::cursor` are the HAL surface.
      The platform halves are `src/platform/windows/window_cursor_win.*` (stock `IDC_*` cursors,
      `WM_SETCURSOR`) and `src/platform/linux/window_cursor_x11.*` (`XCreateFontCursor` cached per
      window, `XDefineCursor`); `url()` lists are `UnsupportedValue` and fall back to `auto`, and an
      unsupported platform reports one `TESSERA_WINDOW_CURSOR_UNSUPPORTED`. Contract in
      `docs/specs/window-chrome-v1.md` §8.5; tests `window_cursor`, `window_cursor_fake`,
      `window_win32`, `window_x11`, `interaction_style`.

### V1-M8 Release readiness ([milestone 8](https://github.com/thnak/Tessera/milestone/8))
Acceptance: a real app ships and passes validation layers and golden tests in CI.

- [ ] #192: Crash capture, symbolication, uploader (metadata in `diagnostics/crash.hpp` landed).
- [ ] #193: Updates and distribution (`docs/specs/updates-and-distribution-v1.md`).
- [ ] #194: Versioning policy for RHI/IR/project config; public API listing gate (#76).
- [ ] #195-#199: i18n/navigation/networking (ADR-052, `docs/specs/networking-v1.md`) as app-framework modules (after M5); security review; release
      CI lanes; memory budgets in CI (#13).

## Build & test

Windows (Vulkan SDK required; `VULKAN_SDK` is set):
```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
.\build\vs2026\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend null
```

Linux:
```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --out batched2d.png
./build/dev/tools/poc/batched2d/batched2d_poc --backend null
```

Formatting and linting:
```powershell
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```
```bash
bash scripts/format.sh --check
BUILD_DIR=build/dev bash scripts/lint.sh
```

The manual `cmake -S . -B build ...` path still works; presets are the recommended route.

## Where things go

| Area | Path |
|---|---|
| Architecture decisions | `decisions/` — start at [decisions/README.md](decisions/README.md) |
| Documentation map | [`docs/README.md`](docs/README.md) |
| Architecture overview | [`docs/architecture/overview.md`](docs/architecture/overview.md) |
| Module map | [`docs/architecture/module-map.md`](docs/architecture/module-map.md) |
| Getting started | [`docs/guides/getting-started.md`](docs/guides/getting-started.md) |
| Developer guide | [`docs/guides/project-generator.md`](docs/guides/project-generator.md) |
| Developer workflow / protected regions | [`docs/guides/developer-workflow.md`](docs/guides/developer-workflow.md) |
| Project config schema | [`docs/specs/project-config-v1.md`](docs/specs/project-config-v1.md) |
| Code style & build cache | [`docs/guides/code-style.md`](docs/guides/code-style.md) |
| Dependency map | [`docs/specs/dependencies.md`](docs/specs/dependencies.md) |
| Public headers | `include/tessera/` |
| Core primitives | `src/core/` |
| Platform seam | `src/hal/` + `src/platform/{windows,linux}` (`macos`, `android`: post-V1) |
| UI model | `src/ui/` (view, layout, style, animation, text, accessibility) |
| RHI (only GPU seam) | `include/tessera/rendering/rhi/` + `src/rendering/rhi/` |
| Batched 2D renderer | `include/tessera/rendering/gpu/` + `src/rendering/gpu/` |
| Null backend | `src/rendering/null/` |
| Vulkan RHI backend | `src/rendering/backend/vulkan/` |
| WebGPU backend | `src/rendering/backend/webgpu/` |
| Software/headless | `src/rendering/software/` |
| Scene graph / events | `include/tessera/ui/scene/` + `src/ui/scene/` |
| Stylesheet spec | [`docs/specs/stylesheet-v1.md`](docs/specs/stylesheet-v1.md) |
| Runtime config | [`docs/specs/runtime-config-v1.md`](docs/specs/runtime-config-v1.md) |
| Runtime loop / pacing | [`docs/design/game-loop.md`](docs/design/game-loop.md) |
| Dirty rendering | [`docs/design/dirty-rendering.md`](docs/design/dirty-rendering.md) |
| Performance / power | [`docs/design/performance.md`](docs/design/performance.md) |
| Window resize/lifecycle | [`docs/design/window-resize.md`](docs/design/window-resize.md) |
| OS events / foldables | [`docs/specs/os-events-v1.md`](docs/specs/os-events-v1.md) |
| Async runtime | [`docs/design/async.md`](docs/design/async.md) |
| Diagnostics/profiling | [`docs/design/diagnostics.md`](docs/design/diagnostics.md) |
| Diagnostics config | [`docs/specs/diagnostics-config-v1.md`](docs/specs/diagnostics-config-v1.md) |
| Input handler | [`docs/design/input.md`](docs/design/input.md) |
| Renderer design notes | `docs/design/renderer.md`, `docs/design/gpu/`, `docs/design/scene-graph.md`, `docs/design/antialiasing.md` |
| UI model / layout / text / animation | `docs/design/ui-model.md`, `layout.md`, `text.md`, `animation.md`, `animation-runtime.md` |
| Responsive / accessibility | `docs/design/responsive.md`, `docs/design/accessibility.md` |
| HAL | [`docs/design/hal.md`](docs/design/hal.md) |
| Mobile integration (post-V1) | [`docs/design/mobile-integration.md`](docs/design/mobile-integration.md) |
| Compiler / IR / component model | [`docs/design/compiler.md`](docs/design/compiler.md), [`docs/specs/ir-v1.md`](docs/specs/ir-v1.md), [`docs/specs/component-model-v1.md`](docs/specs/component-model-v1.md) |
| UI virtualization | [`docs/specs/virtualization-v1.md`](docs/specs/virtualization-v1.md) |
| Designer / Wasm | [`docs/design/designer.md`](docs/design/designer.md) |
| Threading / memory | [`docs/design/threading-and-memory.md`](docs/design/threading-and-memory.md) |
| Memory model / bridges | [`docs/specs/memory-model-v1.md`](docs/specs/memory-model-v1.md), [`docs/design/bridges.md`](docs/design/bridges.md) |
| Buffer config / tunables | [`docs/specs/buffer-config-v1.md`](docs/specs/buffer-config-v1.md), [`docs/guides/tunables.md`](docs/guides/tunables.md) |
| Startup / loading / auth | [`docs/design/startup-and-loading.md`](docs/design/startup-and-loading.md), [`docs/guides/loading-matrix.md`](docs/guides/loading-matrix.md) |
| Resource lifecycle | [`docs/specs/resource-lifecycle-v1.md`](docs/specs/resource-lifecycle-v1.md) |
| Error boundaries | [`docs/specs/error-boundaries-v1.md`](docs/specs/error-boundaries-v1.md) |
| Deep links / activation | [`docs/specs/deep-linking-v1.md`](docs/specs/deep-linking-v1.md), [`docs/guides/deep-linking.md`](docs/guides/deep-linking.md) |
| Automation IDs / headless / audits | [`docs/specs/automation-ids-v1.md`](docs/specs/automation-ids-v1.md), [`docs/specs/headless-automation-v1.md`](docs/specs/headless-automation-v1.md), [`docs/specs/lighthouse-audit-v1.md`](docs/specs/lighthouse-audit-v1.md) |
| Custom native elements / 3D | [`docs/specs/custom-elements-v1.md`](docs/specs/custom-elements-v1.md), [`docs/guides/custom-elements.md`](docs/guides/custom-elements.md) |
| Text editing / forms | [`docs/specs/text-editing-v1.md`](docs/specs/text-editing-v1.md), [`docs/specs/forms-and-validation-v1.md`](docs/specs/forms-and-validation-v1.md) |
| Scroll / gestures / overlays | [`docs/specs/scroll-and-gestures-v1.md`](docs/specs/scroll-and-gestures-v1.md), [`docs/specs/overlays-and-layers-v1.md`](docs/specs/overlays-and-layers-v1.md) |
| Animation / overscroll | [`docs/specs/animation-v1.md`](docs/specs/animation-v1.md), [`docs/design/animation-runtime.md`](docs/design/animation-runtime.md), [`docs/specs/scroll-and-gestures-v1.md`](docs/specs/scroll-and-gestures-v1.md) |
| View transitions (theme/global effects) | [`docs/specs/view-transitions-v1.md`](docs/specs/view-transitions-v1.md) |
| Navigation / restore / shell / i18n | [`docs/specs/navigation-v1.md`](docs/specs/navigation-v1.md), [`docs/specs/state-restoration-v1.md`](docs/specs/state-restoration-v1.md), [`docs/specs/platform-shell-v1.md`](docs/specs/platform-shell-v1.md), [`docs/specs/i18n-v1.md`](docs/specs/i18n-v1.md) |
| Accessibility / async data | [`docs/specs/accessibility-v1.md`](docs/specs/accessibility-v1.md), [`docs/specs/async-data-v1.md`](docs/specs/async-data-v1.md) |
| Devtools / updates / crash | [`docs/specs/devtools-protocol-v1.md`](docs/specs/devtools-protocol-v1.md), [`docs/specs/updates-and-distribution-v1.md`](docs/specs/updates-and-distribution-v1.md), [`docs/specs/crash-reporting-v1.md`](docs/specs/crash-reporting-v1.md) |
| Cross-check / gaps | [`docs/architecture/app-completeness-cross-check-v1.md`](docs/architecture/app-completeness-cross-check-v1.md), [`docs/architecture/app-completeness-gap-resolutions-v1.md`](docs/architecture/app-completeness-gap-resolutions-v1.md) |
| Resources / services / security | `docs/design/resources.md`, `services.md`, `security.md` |
| Testing / goldens | [`docs/guides/testing.md`](docs/guides/testing.md), [`docs/specs/golden-tests-v1.md`](docs/specs/golden-tests-v1.md) |
| Build / packaging | [`docs/design/build-and-packaging.md`](docs/design/build-and-packaging.md), [`docs/guides/packaging.md`](docs/guides/packaging.md) |
| Glossary | [`docs/reference/glossary.md`](docs/reference/glossary.md) |
| Batched2D demo | `tools/poc/batched2d/` |
| Compiler frontend/IR/codegen | `src/compiler/{analyzer,rules,ir,codegen,merger}/` |
| Design-time viewer | `designer/{web,native}/` |
| Tests | `tests/{unit,gpu,integration,golden,fixtures,benchmarks}/` |
| PoCs / demos | `tools/poc/`, `samples/` |

## Code conventions

Style guide: [`docs/guides/code-style.md`](docs/guides/code-style.md) (ADR-015, ADR-038). The
rules below are review-blocking unless a documented exception exists.

- **C++23** ([ADR-038](decisions/ADR-038-cpp23-baseline.md)); `std::expected<T, Error>` is the
  error type for recoverable hot paths.
- **Files:** soft limit 300 lines, hard limit 500; one primary type per header. No exceptions to
  the hard limit except the Phase 0 PoC targets listed in the definition of done.
- **Naming:** `tessera::lower_case` namespaces, `PascalCase` types, `snake_case` functions and
  variables, `snake_case_` members, `kPascalCase` constants, `TESSERA_UPPER_SNAKE` macros.
- **Headers:** `#pragma once`; include what you use; no `using namespace` in headers; no
  Vulkan/WebGPU/third-party headers in public UI headers.
- **Errors/ownership:** exceptions at boundaries, `std::expected` for recoverable hot paths;
  `unique_ptr` for ownership, `shared_ptr` only for genuinely shared lifetime.
- **Hot paths:** no RTTI/reflection, hidden allocations, blocking calls, or per-frame resource
  creation; comments explain **why**, not what.
- **Tests:** every behavior change gets a deterministic test; one component per test file; no
  wall clock, network, system fonts, random ids, or real-GPU baselines.
- **Formatting/linting:** `.clang-format` (LLVM base, 4 spaces, 100 cols, pointer left) and
  `.clang-tidy`. Run `scripts/format.* --check` and `scripts/lint.*`; CI enforces both.
- **Warnings:** `TESSERA_WARNINGS_AS_ERRORS=ON` by default (`-Werror`/`/WX`). Do not add a
  blanket suppression; fix the warning or suppress the narrow third-party source.
- **Build cache:** small translation units, stable public headers, no PCH/unity by default;
  `TESSERA_USE_COMPILER_CACHE=ON` auto-detects sccache/ccache.
- **Project config:** `tessera.json` schema v1 (ADR-014); see
  [`docs/specs/project-config-v1.md`](docs/specs/project-config-v1.md) and
  [`docs/guides/project-generator.md`](docs/guides/project-generator.md).
- **Shaders:** GLSL → SPIR-V via `glslc`; SPIR-V is the source of truth for Vulkan. No runtime
  shader compilation in production.

### Documentation rules

Documents are part of the deliverable, not an afterthought. One topic per file; normative rules
live in one place and are linked, not copied.

| Type | Location | Purpose |
|---|---|---|
| Architecture | `docs/architecture/` | cross-cutting structure, ownership, gaps |
| Normative spec | `docs/specs/` | exact contracts, schemas, invariants |
| Design note | `docs/design/` | rationale, subsystem design |
| Guide | `docs/guides/` | task-oriented instructions |
| Reference | `docs/reference/` | glossary/terms |
| ADR | `decisions/` | contentious decisions and consequences |

- **Size:** target 400 lines, hard limit **600 lines**. A topic over 600 lines becomes a folder
  with a `README.md` index and linked parts (see [`docs/design/gpu/`](docs/design/gpu/README.md)).
- **Index:** every new, moved, or renamed doc updates [`docs/README.md`](docs/README.md); every
  new ADR updates [`decisions/README.md`](decisions/README.md).
- **Links:** use relative links; headings are stable; no orphan docs; code fences must balance.
- **Single source:** specs are normative, design docs explain, ADRs record decisions; do not
  duplicate a rule in multiple docs.
- **New subsystem:** add a design note; add an ADR if the decision is contentious or hard to
  reverse; update the versioning spec when a new artifact/format is introduced.
- **Change scope:** docs-only changes still need the link/index/fence/size checks; behavior
  changes update the affected docs and tests in the same change.

## Definition of done (every change)

- [ ] Builds with `TESSERA_WARNINGS_AS_ERRORS=ON` on Windows and Linux, and the WebGPU/Wasm lane
      stays green (the gating lanes of "V1 platform scope and CI policy"; macOS, Android and iOS
      are advisory).
- [ ] Unit/integration tests pass (`ctest`).
- [ ] Vulkan paths run clean under validation layers (debug builds).
- [ ] No new dependency without entries in `docs/specs/dependencies.md` and
      `THIRD_PARTY_NOTICES.md`.
- [ ] Static link check passes: no unexpected third-party dynamic libraries (ADR-012).
- [ ] New source files carry `SPDX-License-Identifier: MIT`.
- [ ] `scripts/format.* --check` passes; clang-tidy reports no new warnings in changed files.
- [ ] Files stay under the 300/500-line soft/hard limits (ADR-015). Phase 0 PoC targets
      (`tools/poc/vulkan_offscreen`, `offscreen_renderer.cpp`) are exempt until retired.
- [ ] Docs are updated/indexed; relative links and code fences are valid; no doc exceeds the
      600-line hard limit (target 400). New ADRs are in `decisions/README.md`.
- [ ] Generator/config changes update `docs/guides/project-generator.md` and the schema.
- [ ] If it touches protected regions: idempotency test updated.
- [ ] If it touches rendering: a golden test or a manual visual check is recorded.
- [ ] If it touches dirty state: dirty-only and full-redraw output are equivalent.
- [ ] If it touches pacing: frame-cap/vsync/VRR fallback behavior is recorded.
- [ ] If it touches power/idle: a static scene produces zero presents/submissions after settling.
- [ ] If it touches window/resize: synthetic resize/DPI/lifecycle tests pass per platform. On a machine with a GPU, also run `scripts/gpu_tests.ps1 -Repeat 3` (hosted CI skips the window tests; see `docs/guides/real-gpu-ci.md`).
- [ ] If it touches OS events/foldables: synthetic fold/posture/display scripts pass.
- [ ] If it touches async: no blocking UI wait; cancellation/shutdown/queue-overflow tests pass.
- [ ] If it touches diagnostics: disabled paths add no measurable cost; redaction tests pass.
- [ ] If it touches buffers/tunables: schema/min/max/step/budget validation passes; user
      settings are bounded; golden output is unaffected by buffer sizes.
- [ ] If it touches startup/loading/auth: budgets and failure/offline paths are tested; no UI
      thread blocking; loading/auth screens are accessible.
- [ ] If it touches resources: lazy/preload/lease/unload/eviction tests pass; no unload of
      active/pinned resources; no leak/thrash under route churn.
- [ ] If it touches error boundaries/exceptions: fallback/retry/nested/async/event/fallback-
      failure/loop/redaction/cleanup tests pass; no throw across threads/FFI; no silent catch.
- [ ] If it touches deep links/activation: cold/warm/single-instance/auth/malformed/oversized/
      file-association/browser-fallback tests pass; no secrets in URLs/logs; no shell execution.
- [ ] If it touches automation/headless/audits: numeric ID manifest is deterministic; snapshot/
      action/trace/audit schemas pass; golden/GPU separation; audit disabled adds no cost.
- [ ] If it touches custom elements/3D: manifest/prop/event/shader validation, compile-time
      registry, leaf/reuse rule, element input/capture/custom events, offscreen/inline and
      unit/structural/golden/GPU-smoke tests pass; no raw backend/OS handles or runtime shaders.
- [ ] If it touches text editing/forms: grapheme/bidi/IME/selection/undo/clipboard/secure fields,
      sync/async validation, focus-first-invalid, accessibility and headless tests pass.
- [ ] If it touches scroll/gestures/overlays: nested scroll/gesture arena, snap/sticky/anchoring,
      modal focus/back/dismiss, anchoring/clamping and goldens/headless tests pass.
- [ ] If it touches animation/overscroll: fixed-step sampling, reduced motion, interrupt/retarget,
      overscroll resistance/spring-back, no offset/layout/ID change, goldens/headless tests pass.
- [ ] If it touches view transitions/global effects: lifecycle/interrupt/skip-reason, reduced-motion/
      power downgrade, texture budget/leak, manifest validation, goldens at fixed progress, and
      zero presents after finish pass; no runtime shaders.
- [ ] If it touches navigation/restore/shell/i18n/a11y/data: route/back/deep-link/restore
      versioning, clipboard/DnD/dialog permissions, locale/RTL, semantics, cache/offline tests pass.
- [ ] If it touches devtools/updates/crash: local-only devtools, signed/staged/rollback updates,
      symbolicated/redacted crash reports, and release-disabled devtools tests pass.

## Current focus

**Implemented on `main`:** the B1 renderer (RHI, null and Vulkan backends, batched 2D, scene
graph); the B2 runtime (fixed-step loop, clocks, pacer with scripted refresh changes, async
executors and tasks, input with focus and capture, dirty flags, damage and redraw plans, per-node
paint caches, the stylesheet engine with layers/media/supports, Yoga layout, the
FreeType/HarfBuzz/SheenBidi text stack with an opt-in ICU, the animation and transition runtime with
springs and end events, structured diagnostics, lavapipe and CPU goldens, virtualization with a
scene-side scroll event, automation ids and the headless harness); B3 windows and swapchains on
Win32 and X11 with device-loss recovery, host-owned caret blink, occluded/minimized pause and a
single timeline semaphore pacing frame submits; adaptive quality end to end (controller, render-scale
consumer with a bit-identical scale-1.0 path, present-cap divisors over a real HAL display-refresh
query); the Big Step C compiler (TSX front end, IR, codegen, forms, virtual lists, animated values,
cross-file props type aliases, stable component identity, synthesized vector payloads with a
composable asset bundle);
both ADR-048 blob artifacts (compiled stylesheets registered in import-graph order under
`@layer tessera-ui`, and the embedded theme asset loaded without parsing); in-tree UIA and AT-SPI2
bridges not yet installed in a host; and the first WebGPU slice (unbuilt on host).

**Farm order:** V1-M1 and V1-M2 first. Remaining unblocked M2 slice: the ADR-040 generated-base
migration (the last #70 leftover, object-literal prop defaults, is blocked on an IR record type).
Remaining M1 slices, in order of what is actually startable: the B3 Wayland window, then the two
items waiting on a definition — the remaining power steps
([#147](https://github.com/thnak/Tessera/issues/147)) and the interpolation alpha threshold
([#148](https://github.com/thnak/Tessera/issues/148)); the B3 OS-event item still waits on
ADR-023. V1-M3 and V1-M5 wait for their design PRs
(#83/#84 ADR-049 and shader chain, #85 ADR-050); only ADR-050 AF1 may start now. V1-M4, V1-M6, V1-M7
and V1-M8 come later.

**V1 gaps added 2026-11-13 (ready now):** #226, #227, #229, #230, #231 and #235 (front end, style and
paint gaps that make V1 look unfinished), #228 (multi-window refactor, before AF6), #232, #233,
#222, and the library stage 0 items #234, #237 and #238. #226, #229 and #231 touch the IR and the
front end, so land them one at a time. Stage 0 is closed (#239), so #236, #240 and #223 are unblocked.
#218 and #219 run one family or step at a time (they share `src/compiler/codegen/CMakeLists.txt`).

The detailed history of what landed and why is in
[`docs/architecture/implementation-log.md`](docs/architecture/implementation-log.md). Keep the B1
RHI, batched 2D renderer and scene graph stable.
