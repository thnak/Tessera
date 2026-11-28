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
   compile error. This is the only codegen path: the legacy struct output, its
   `[DEVELOPER_LOGIC_START/END]` regions, the merger and `EmitMode` are deleted (#209, #210, #218,
   #219), so `generate()` has no mode and the CLI has no `--mode`. The legacy emitters survive only
   as the text source `emit_base.cpp` reshapes into the base.
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
- **Proposed on purpose (owner direction).** An ADR may stay Proposed while its items land, when the
  owner says the decision is expected to move during V1 and locking it would be premature. The owner
  records that direction in the ADR's status line, naming the date and the reason; the items then
  carry `ready` and land normally. This is not a general licence to skip Acceptance: it applies only
  to an ADR whose status line says so, and the ADR is still amended by evidence as its items land.
  ADR-066 is the first such case (owner direction 2026-11-22: the React core set may change as V1
  ports real code, so it stays Proposed and its steps #651 to #654 are open).
- **Spikes:** an item labeled `spike` may depend on a Proposed ADR. It proves one decision with
  measured evidence: it lands off by default (an experimental flag or CMake option) and changes no
  default behavior, its PR appends the requested evidence and what the ADR got wrong to the ADR's
  "Evidence required before Accepted" section, and it never changes the ADR's status. Only the owner
  moves an ADR to Accepted; the items that implement it wait for that, then the sweep below labels
  them `ready`.
- **One landing per PR**, branched from `origin/main`: a whole issue, a whole family or step of a
  multi-part issue, or several related issues together (`Closes #a, #b`). Attach the milestone
  (`gh pr create --milestone "V1-M1 Core runtime"`) and tick each item's checkbox in this file in
  the same PR (`[x]` plus the evidence path). Size rules are in the next subsection.
- **Close or split, never leave open:** the PR body names every issue it finishes as `Closes #n`
  (one per issue, so GitHub closes each on merge). Write it as plain text: `Closes **#199**` or
  `Closes [#199](...)` is not parsed and the issue stays open (#371 did this). An issue the PR only partly finishes is not
  closed and not left as is: either narrow its body to the remaining work in the same PR's
  description and comment on the issue, or open a follow-up issue for the remainder, then close the
  original. A merged PR whose issue stays open with its original text makes the next agent redo it.
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
- **Promote unblocked issues:** nothing else flips waiting issues to `ready`, so the agent whose PR
  merges does it, and an agent that finds no `ready` issue runs the same sweep before stopping.
  List open issues with none of the three labels
  (`gh issue list --state open --json number,labels,body`), read the "Depends on", "After", "Waits
  on" and "Needs" lines in each body, and add `ready` only when every named issue is closed
  (`gh issue view <n> --json state`) or the line says the work can start now. Leave an issue
  unlabeled when it is a question or owner decision, needs a measurement first, targets an advisory
  platform lane, changes CI workflows (verifiable only by pushing), or has a dependency you cannot
  confirm; if unsure, comment on the issue instead of labeling it.
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
- [x] #156: **B3:** OS events (ADR-023, accepted): first slice is the normalized `OsEvent`/`WindowMetrics`/`FoldInfo`
      types, a synthetic event injector, and display-capability events on Windows and X11 (DPI, scale,
      refresh, work area) — synthetic display scripts pass; fold/posture/hinge backends are post-V1
      (Android). The taxonomy is `include/tessera/hal/os_event.hpp` (48 `OsEventType` enumerators,
      category/order/coalescing/wake rules, `kOsEventTypeCount` compile-checked against the rules
      table) with `display.hpp` (`DisplayId`, `DisplayInfo`, `Orientation`), `event_reason.hpp` and
      `synthetic_events.hpp`; the platform-neutral halves are `src/hal/window/{os_event,display,
      event_reason,window_events,synthetic_events}.cpp`, and the window path projects onto the
      taxonomy through `os_event_type_for` (total, `static_assert`-checked table). `WindowMetrics`
      grows the display fields (`display`, `refresh_hz`, `work_area`, `orientation`, VRR/PiP, each
      with an `*_assumed` flag per ADR-023 rule 7) plus six new `WindowEventType` values
      (`RefreshRateChanged`, `WorkAreaChanged`, `DisplayAdded/Removed/Changed`, `ScaleChanged`); the
      struct is byte-packed and guarded by `kWindowMetricsSizeBudget` (384) with a `static_assert`.
      The platform halves are `src/platform/windows/window_display_win.cpp` +
      `window_display_messages_win.cpp` + `window_dpi_win.cpp` (per-monitor reads via
      `GetDpiForMonitor`/`shcore`, mode + work area, the display/settings message classification)
      and `src/platform/linux/x11_display.cpp` + `x11_randr_monitors.cpp` (`Xft.dpi`, `_NET_WORKAREA`,
      RandR monitors/modes, RandR event normalization), all split out so neither window TU grows.
      Evidence: `os_event` (`tests/unit/os_event_tests.cpp`) covers the taxonomy, ordering,
      coalescing, staleness, the total window-path mapping and the 48/18 counts; `os_event_display`
      (`tests/unit/harness/os_event_display_tests.cpp`) drives the four synthetic scripts (resize/DPI
      storm, hotplug, scale change, refresh change) through `SyntheticEventInjector` and the fake
      window's `poll_events`, asserting the recorded 60 Hz/1.0 assumptions and that an unknown event
      is ignored without a crash. Verified on Windows (Release, `/WX`) and Linux (`dev`): both build
      clean and the window/display/metrics/event suites pass; the full Linux failure set is identical
      to the pristine baseline (no Vulkan validation layer on that box). Not exercised: live
      hotplug/DPI on real second-monitor hardware (synthetic scripts are the deterministic evidence),
      and Wayland display capabilities (no producer in this slice).
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
      sampled target from #254) — `src/rendering/gpu/glass_compositor.cpp` (formerly
      `batched2d_glass.cpp`; `GlassCompositor::compose` via `Batched2DRenderer::draw_backdrop`:
      break -> region copy -> horizontal blur -> resume ->
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
- [x] Style and paint gaps found by the tactile design mock and the Astryx theme model, one landing
      each (Chromium-measured, golden on lavapipe, property-status doc updated): #617 conic gradient,
      `mask`, `filter` (decided: conic gradient, then `mask`, then `filter`; a live `Path` is a
      rasterizer project, not a paint primitive —
      [`dials-and-gauges.md`](docs/design/dials-and-gauges.md)). All three trails have landed; see
      the #617 entry below for the evidence and the one remaining alias gap. Umbrella #618.
- [x] #613 `radial-gradient()` and the repeating forms — one grammar with two shape halves, radial
      painting by bounded 32×32 tessellation through the shared sink hook (so CPU and GPU emit
      identical geometry), repeating as a stop-list transform, gradient interpolation wired into
      keyframes — `src/ui/style/gradient_radial.cpp`, `src/ui/paint/paint_gradient.cpp`,
      `tests/unit/gradient_interpolation_tests.cpp` (PR #675). A layer is a whole `GradientValue`, so
      #613 and #614 compose in one stack: the paint path walks the layer list bottom to top and
      resolves each layer into the sink's own draw type.
- [x] #614 several background layers — `background-image` is a comma-separated layer list painted
      bottom to top, the `background` shorthand puts the colour on the final layer only, `url()` is
      rejected with a diagnostic — `src/ui/style/background_layers.cpp`,
      `tests/unit/background_layers_tests.cpp` (PR #676).
- [x] #617 conic-gradient, `mask` and `filter` (dials and gauges) — decided 2026-11-20 in
      [`dials-and-gauges.md`](docs/design/dials-and-gauges.md): conic gradient, then `mask`, then
      `filter`; **no `Path` route in V1** (a live `Path` needs paint-time rasterization and an
      animatable `d`, which is a project). Two of the three trails are done:
      **conic** (#680: `repeating-conic-gradient()`, 64×64 cell tessellation within 5/255 of
      Chromium); **mask** (`mask-image` on isolated layers, RHI 1.19 `DestinationIn`; the
      `mask-size`/`position`/`repeat`/`mode`/`origin`/`clip` longhands and the `mask` shorthand;
      `mask-composite` with RHI 1.21 `SourceOut`/`Xor`; the CPU twin and the lavapipe golden);
      **filter** (`blur()` and `drop-shadow()` on isolated layers, its own node field, subtree flag
      and damage walk). Each justified a renderer change, not a new shader: the offscreen layer pool
      now recycles inside the frame, caps bytes and trims idle, with refusals reported through
      `diagnostics::OffscreenPressureReporter` (PRs #928-#941, `runtime-config-v1.md` `offscreenMaxMb`
      / `offscreenIdleSeconds`). The eight filter **colour functions** (`brightness`, `contrast`,
      `grayscale`, `sepia`, `saturate`, `hue-rotate`, `invert`, `opacity`) landed in #942: measured in
      Chromium, packed up to three per `LayerFilterOp`, one 1:1 pass of the existing blur program
      (mode 3), no new RHI feature, and the CPU twin runs them too. A masked, filtered or
      isolated-opacity group is now **retained** across frames instead of re-rendered (#944: the
      layer target stays bound until a dependency dirties, with `layer_retain_*` and
      `paint_layer_retain` tests and a bench). The `-webkit-mask*` family closed it: the eight
      prefixed longhands are registry aliases of their canonical names (one computed value, folded at
      the expand step so the two spellings cannot fight in the cascade, the `-webkit-backdrop-filter`
      pattern), and the prefixed **shorthand** is mapped to `mask` *before* the registry dispatches,
      so it goes through the same layer parser and the same eight longhands rather than falling
      through to the single-value path. ctest `webkit_mask_alias` (the prefixed and canonical
      expansions are equal, including for values `mask` rejects) and the schema golden's eight new
      rows. All three trails are therefore closed.
- [x] #622 `:has()` and `@scope` — both supported, no named exception: `@scope` styles a component's
      subtree without a class on every descendant and `:has()` lets a rule reach a nested region it
      hosts, which is what ADR-064 item 6's nested theme region needs; the `:has()` walk is bounded
      by the subtree it visits and the index still narrows by subject compound first —
      `src/ui/style/selector_has.cpp`, `docs/design/theme-scoping.md`,
      `tests/unit/theme_scoping_tests.cpp` (PR #676).
- [x] #620 `light-dark()` and #621 `color-mix()` — computed-value-time colour functions that resolve
      inside any value, `color-scheme` as the inherited branch selector, four interpolation spaces
      with premultiplied alpha — `src/ui/style/color_functions.cpp`, `tests/unit/color_functions_tests.cpp`
      (PR #665).
- [x] #612 inset `box-shadow` — the mirror of the outer shadow: painted inside the rounded border box
      above the background and below the content, one quad with the coverage inverted
      (`kInsetShadowModeBase`), the flattening sinks get the inverse strips —
      `src/ui/paint/paint_shadow.cpp`, `src/rendering/gpu/batched2d_shadow.cpp`,
      `tests/unit/paint_shadow_tests.cpp`, `tests/gpu/paint_shadow_cpu_tests.cpp` (PR #666).
- [x] #616 `text-shadow` — parsed, painted behind every glyph run of a block (a sink wrapper buffers
      the runs so a later line's shadow never darkens an earlier line's glyphs), interpolated, and
      layout/ink-box invariant — `src/ui/paint/paint_text_shadow.cpp`,
      `tests/unit/paint_text_shadow_tests.cpp` (PR #667).
- [ ] #615 `clip-path` basic shapes — [ADR-068](decisions/ADR-068-clip-state-in-a-storage-buffer.md)
      (Accepted) moved clip state into a per-frame storage buffer so `ellipse()`, elliptical `inset()`
      radii and `polygon()` can reach the fragment shader with a CPU reference both agree on.
      Landed as library slices: #669 RHI 1.12 null + Vulkan, #671 `ClipStateTable` and shader on the
      buffer, #672 `circle()`/`ellipse()`/`inset()` (parser, shader, `CpuCanvas` shaped clip), #673
      `polygon()` with both fill rules (vertex pool, `Batched2DRenderer::push_clip_shape`; PR #911) and
      #674 shape interpolation (`animation/clip_path_interpolation.cpp`; PR #911). #670 landed: WebGPU storage buffers (#905) and Metal
      (PR #953: `bind_storage_buffer`, per-program fragment slots, the conformance check passes on the
      macOS runner; analytic gradients stay off on Metal). Open: the wiring itself — the paint pass reading `clip-path` (a shaped
      clip around a node's children) and hit testing against the shape. `path()` stays out of V1
      (fill-rule evaluation, no layer budget).
- [ ] Behavior hook engine and core hooks (ADR-064 item 7; waits for ADR-064 Accepted, after spikes
      #658 and #662): engine interface, registry and `press_feedback` #640, then #624 to #635.
- [ ] GPU for CPU-produced pixels (measured 2026-11-21 at 4K on the RX 5300M and the iGPU:
      ThorVG raster 14.9 to 30.9 ms, upload 4 to 6 ms CPU side, image resample 50 to 57 ms per
      miss; budget is 16.7 ms at 60 Hz, 4.2 ms at 240 Hz). Hypotheses are checked before an
      issue is `ready`: the per-call staging allocation was refuted as the upload cost (reuse
      saves under 2 ms; the memcpy is the cost), the `create_texture` tail is VMA block growth
      (60 ms per added block on the dGPU, a fresh block per 4K image on the iGPU), so #681 is
      rescoped to `write_region`, dirty-rect atlas uploads and a device-memory pool/pre-warm;
      hardware nearest sampling is not bit-exact with the CPU rule (wrong neighbor on every pixel
      at 2:1), so #682 (no CPU image resample) uses integer `texelFetch` nearest. Both GPUs
      measured are AMD; other vendors are untested. Vector paths go to a GPU compute
      tile rasterizer, owner direction "best performance, cost is not a criterion": #683 ADR + 4K
      spike (`spike`, amends ADR-057; full re-encode crosses 0.5 ms at about 28k segments, so
      GPU path generation is justified above roughly 30k segments), then #684 RHI 1.10 compute
      (waits for #683 Accepted and #669), then #685 the GPU path renderer (waits for #683 and
      #684), then #686 the glyph rasterization measurement. Measurement tool:
      `tests/gpu/batched2d/cpu_to_gpu_4k_measure.cpp`.
      Landed 2026-11-27 (PR #951): #685 the GPU path renderer — scale-independent documents
      (`GpuPathDocument`, cached by geometry), a device-resident arena for fill edges, strokes and dashes,
      the `PathRenderer` seam (`GpuPathRasterizer` implements it; `PathTextureCache::set_path_renderer`),
      the 27-scene corpus with a ThorVG tolerance table and lavapipe goldens
      (`tests/gpu/batched2d/path_raster_*`, `tests/golden/gpu/path_raster/`), the WGSL twin on WebGPU
      (Dawn and real Chrome), and `gpu_path_animated_measure`: RTX 4060 4K, CPU 0.04 ms per frame,
      GPU 1.25 ms sparse and 3.2 ms dense, 7.8 KB and 912 B written per frame (not literally zero;
      ADR-071 says why). #947 fixed (zero-length strokes). #686 measured (`docs/design/text-gpu-raster.md`
      section 6; follow-ups #948 and #949 landed, #950 open). Left: #689 (the GPU downscale was rejected on numbers).


### V1-M2 Compiler and animation ([milestone 2](https://github.com/thnak/Tessera/milestone/2)) — Big Step C
Acceptance: a `useState`/`useEffect`/`useRef` screen regenerates without losing developer C++.

- [x] JSX/TSX front end + Tier-1 rules — `src/compiler/{analyzer,rules}/`.
- [x] `bind::` extractor + IR — `src/compiler/{ir,lowering}/`.
- [x] C++ codegen (one path: the ADR-040 generated base) — `src/compiler/codegen/`,
      `tools/codegen/`.
- [x] Acceptance — `samples/compiler-demo/Demo.tsx` plus its developer class
      `samples/compiler-demo/Demo.cpp`, regenerated with the developer class untouched.
- [x] Animated values (tween, spring, `onComplete`, reduced motion live on Windows), CSS end events,
      `SpringRuntime` in the frame pipeline — `src/ui/animation/`, commits `a76165f`, `a075170`.
- [x] #157, #209, #210, #218, #219 (one canonical codegen path): ADR-040 emits
      `gen::<Component>Base<Self>` plus a scaffolded developer class; the legacy output, the
      merger and `EmitMode` are deleted — `src/compiler/codegen/src/emit_base.cpp`,
      `tests/unit/codegen_base_tests.cpp`, `tests/golden/codegen/*.g.hpp`.
- [ ] #211 (docs describe the one path) — the follow-up documentation slice.
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
      the IR record type + spec landed (ADR-051 slices 1-2:
      `src/compiler/ir/include/tessera/compiler/ir/types.hpp`, `IrModule.types` sorted by name,
      `ExprKind::Record`/`make_record`, `src/compiler/ir/src/{types,validate_record,
      validate_record_value}.cpp`, spec `docs/specs/ir-v1.md` §2.2/§4.3 with §9/§9.1/§12 in
      `docs/specs/ir-v1-expr.md`, `irVersion` stays 1 and a module without records is byte-identical,
      tests `ir_record_writer`/`ir_record_table`/`ir_record_value`).
      Still to do: object-literal lowering for `useState` initials and prop defaults (ADR-051 slice 3),
      then codegen struct + designated initialization (slice 4, after the #218/#219 codegen migration)
      and evaluator parity (slice 5).
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
- [x] Spikes that decide the Proposed ADRs 064 to 067 (each lands off by default and records
      evidence in its ADR): #658 behavior hook vertical slice, #659 custom hook expansion on a
      fixture corpus, #660 React port corpus measurement, #661 Suspense and transition on a fake
      resource. #658 done: the engine seam, the registry, `press_feedback`, the built-in hook
      manifest reader, the analyzer, IR `behaviors`, the generated base's behavior surface, the
      shared `behavior::Instance` and the preview wiring all land off by default (PRs #678, #679 and
      the #658 landing). The spike found three defects that made the path unreachable — the lowering
      dropped `component.behaviors`, a behavior output had no `beh::<name>.<field>` reference form,
      and the evaluator's scope had no `beh::` entries — and one spec rule that was wrong: a
      hash-only `update` never fires for a behavior with no options, so the seam has a second
      scene-driven `refresh` trigger. Evidence in
      [ADR-064](decisions/ADR-064-astryx-style-component-system.md) and
      [`behavior-hooks-v1.md`](docs/specs/behavior-hooks-v1.md) sections 8 and 10. #662 done: a role-token theme source builds to plain CSS custom
      properties in `@layer` and the engine's own cascade resolves it in light, dark and high
      contrast (`theme_source`, `docs/specs/theme-source-v1.md`); the evidence and the three engine
      gaps are recorded in ADR-064 item 6.
- [ ] Behavior hooks in the front end (ADR-064, waits for Acceptance): spec #638, analyzer and lowering
      #639, code generation #641.
- [ ] Custom hooks by inlining (ADR-065, waits for Acceptance): #645 expansion pass, #646
      diagnostics, #647 typings, member names and preview state carry, #648 documentation sweep.
- [ ] React core additions (ADR-066, Proposed on purpose per owner direction 2026-11-22, so its
      items are open): #651 cheap acceptances **done** (arrow components, `export default function`,
      `import React`, `memo`, `StrictMode`/`Profiler`, `useId`, `useLayoutEffect`, the safe
      `useMemo`/`useCallback` forms, `??`/`?.` as the additive `coalesce`/`optionalField` IR kinds,
      and the `id`/`aria-labelledby` schema pair `useId` needs; corpus 25→33 front end, 12→20
      pipeline; evidence in ADR-066 "Step 1 evidence"), #652 control flow in handlers **done**
      (`if`/`else` as the additive `If` action reusing `guard`, `else if` nested, a handler `const`
      inlined, an early `return;` ending its list; corpus 33→34, 20→21; evidence in ADR-066 "Step 2
      evidence"), #653 list and string expressions **done** (`filter`/`slice` as the additive
      `listFilter`/`listSlice` kinds, `.length` as the existing `length(...)` call, the total string
      methods as calls with the receiver first, and `{ ...state, x }` as the additive `recordUpdate`
      kind; corpus 34→38, 21→24; evidence in ADR-066 "Step 3 evidence"), #654 limited
      `useReducer` and static `useContext` **partly done**: the reducer form parses (`switch`/`case`/
      `default` as a statement form, a two-parameter module-scope helper, and a helper whose body
      declares a `switch` is parsed rather than skipped), the form is recognised
      (`TESSERA_REACT_REDUCER_FORM` for anything outside it), and it lowers to one state cell plus
      one handler per action type with a dispatch table, so `dispatch({ type: "x" })` is a
      `Dispatch` action. Corpus 38→39, 24→26. `useContext` is still rejected, and the generated
      C++ for a reducer state is the next slice: codegen has no record table in its text-value
      typing, so `{state.count}` reports `TESSERA_CODEGEN_UNSUPPORTED` even though the IR is valid.
- [ ] Pending UI (ADR-067, Accepted 2026-11-28): spec #655, front end and engine behaviors #656
      (preview support #657 belongs to V1-M4). #656 slice 3 landed: `useResource` (typed scalar
      `.data`, `.status`), `useDeferredValue`, `useTransition` end to end from TSX through the
      evaluator and generated C++ (`codegen_pending_tsx`), the boundary keeping its old subtree
      while a transition is pending, and a fix for Suspense fallback ordinals (a binding after a
      boundary got its neighbour's mask). Left before the box can be ticked: `.error` has no
      generated form, the data-store adapter publishes no typed `.data`, a resource in an effect
      dependency list is rejected, and the host (not generated code) binds a `ResourceView` per
      resource. Decisions 10 to 15 of `docs/specs/pending-ui-v1.md` section 12.


### V1-M3 Backends and shader chain ([milestone 3](https://github.com/thnak/Tessera/milestone/3))
ADR-049 is Accepted and the shader chain is on `main`; WebGPU is Tier 1: its CI lane (`wasm-build` + `wgsl-validation`) is green on GitHub (promoted 2026-11-13).

- [x] WebGPU first slice (mapping tables, WGSL registry, unbuilt skeleton) —
      `src/rendering/backend/webgpu/`, `tests/unit/webgpu_mapping_tests.cpp`.
- [x] #159: `shader_id`, portable shader branch, opt-in cross-compile — PR #84 plus the closing
      slice: the normative id-to-source table and the identity rule in
      [`docs/specs/shader-manifest-v1.md`](docs/specs/shader-manifest-v1.md) section 3.2, the
      opt-in section 5.1, and the `shader-cross` lane in `.github/workflows/linux.yml` that runs
      `TESSERA_SHADER_CROSS=ON` (10 `shader_cross_*` + `shader_parity`, `dxc` skipped on Linux);
      the lane found and fixed a real defect — Linux glslang 16.1 lowered the by-value
      `ClipUniform` parameter to `OpCopyLogical`, which naga rejects, so
      `src/rendering/backend/vulkan/shaders/batched2d.frag` indexes the clip block instead
      (Vulkan pixels unchanged: `batched2d_golden`, `batched2d_shapes_golden`, `rounded_clip`,
      `glass_composite_gpu`, `damage_equivalence` pass). Cross tools in
      [`docs/guides/writing-a-backend.md`](docs/guides/writing-a-backend.md) section 7.1.
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
- [x] #245: Cross-backend image parity: a fixed scene set on every available backend against the
      lavapipe reference, SSIM and deltaE with edge-rounding vs structural classes, tolerances per
      backend pair in `tolerance.json` — landed in PR #261 (`tests/gpu/backends/`,
      `include/tessera/rendering/golden/parity.hpp`).

### V1-M4 Design-time preview and playground ([milestone 4](https://github.com/thnak/Tessera/milestone/4)) — Big Step D
Acceptance: a screen previews in a browser and matches native output within the golden tolerance.
Stages P0-P7 are planned in `docs/design/reference-equantic-ui.md`.

**This is the design-time milestone**: everything a developer uses before the app ships (the browser
preview, the studio that edits a project folder, the MCP adapter, devtools). Runtime and shipping
concerns live in V1-M5 and later. Since 2026-11-16 the preview pages are merged into one
studio `index.html` (#392, PR #420), and since 2026-11-17 that shell is React 19 + Vite (ADR-054,
#422); the folder IDE (#391) and the workbench (#584, W0-W5 landed in PR #586) build on it. The
order below is the farm order. #397-#399 (stack overflow and quadratic lowering on untrusted input,
from the #198 review) are closed, so #390, #391 and the MCP folder tools can feed untrusted files to
that code.
The Wasm studio stays single-threaded (ADR-045 decision 5); a separate multi-threaded studio site is a later,
owner-planned item and is not part of this milestone.

**State on 2026-11-27:** every P0-P7 stage, the studio shell, the folder IDE's IO layer, live edit,
device profiles, the workbench dock and the studio-side mock services have landed. What is left in
this milestone is the IDE work listed under #584 (W5b-W9), #516, #567-#569 and #575-#583, the canvas
viewport work (#690) and the remaining MCP tools (#396). The fold-hinge and safe-area overlay (#433)
has landed: the Canvas pane's **Overlay** toggle shades the hinge band, the insets and the cutouts the
running tree was laid out inside, drawn from `tessera_canvas_overlay` - a query over the session's own
`WindowMetrics`, so the band cannot disagree with the layout it explains (`tools/codegen/
canvas_overlay_report.*`, `designer/web/src/views/canvas-fold*.ts`; gates `canvas_overlay` ctest,
`preview_fold_overlay.mjs`, `preview_browser_fold_overlay.mjs`). The paint
family is complete: #617 closed with the `-webkit-mask*` alias family, after #942's filter colour
functions and #944's retained layers.

- [x] #164: P0: Emscripten preset, compiler in Wasm — IR byte-identical to native for
      `samples/parity-demo/Counter.tsx`. Evidence: `wasm-compiler` build preset (the Emscripten
      `wasm-webgpu` configure with a compiler-only target set), the CLI's Emscripten-only
      `-sNODERAWFS=1 -sENVIRONMENT=node` link options (`src/compiler/codegen/CMakeLists.txt`), the
      gate `scripts/ci/wasm_ir_parity.py` (IR plus generated C++, byte-for-byte), the `wasm-parity`
      job in `.github/workflows/webgpu.yml`, and `docs/design/compiler-wasm.md`. Both sides emit
      4542 bytes of IR with SHA-256 `fa9989b1...f52fa9`; three repeated runs reported
      `unique_digests=1`, and the four generated C++ files match byte-for-byte too.
- [x] #165: P1: diagnostics-only page on GitHub Pages — diagnostics match native. Live at
      https://thnak.github.io/Tessera/, deployed by CI from `main`; the parity gate is
      `scripts/ci/wasm_diagnostics_parity.py` and the format is
      `docs/specs/compiler-diagnostics-v1.md`.
- [x] #166: P2: evaluator (ADR-037) + scene + layout on the null RHI — snapshot equals native
      harness. The shared walk is `src/ui/automation/scene_snapshot.cpp` (the harness delegates
      to it), the gate is `scripts/ci/scene_snapshot_parity.py` (11 cases), and
      `docs/design/compiler-wasm.md` section 7 records it.
- [x] #167: P3: WebGPU canvas preview — the native half, the page and the device plumbing.
      `tools/codegen/{scene_prepare,scene_frame,canvas_report}.*`, a canvas `RenderTarget` over a
      host-owned `WGPUTextureView`, `designer/web/canvas.*`, and the `canvas-native-parity` job that
      renders the lavapipe reference. The **pixel comparison remains open**: it needs a browser+GPU
      CI dependency (an owner decision) and `tests/golden/canvas/` produced on the lavapipe lane,
      which `--write-goldens` refuses to create from any other device. #386 then fixed what the first
      browser run found: the device is handed over as Emscripten handles (`WebGPU.importJsDevice`),
      the frame is presented on the page canvas, a failed frame clears the canvas instead of
      showing black, and the WebGPU command list no longer casts a canvas target to the offscreen
      class. Text, pointer input and the pixel lane are the follow-ups below.
- [x] #168: P4: browser input into the evaluator, state kept across edits — a cell is carried
      when the component id, cell id and IR type all match (`tools/codegen/scene_state.cpp`),
      dispatch is the unmodified `ui::InputRouter`, and the gate is
      `scripts/ci/scene_input_parity.py`.
- [x] #169: P5: shareable links, sample gallery, theme/DPR/viewport toggles —
      `designer/web/{share,preview-state,gallery,studio,scene-input}.js`, the generated
      `designer/web/samples.json`, 18 + 9 + 7 page test cases, and the format in
      `docs/specs/designer-studio-v1.md` section 7.1 (share links, the gallery and `samples.json` since removed).
- [x] #170: P6: size and latency budgets with a two-sided ratchet —
      `scripts/check_preview_budget.py` + `tests/golden/perf/preview_budget.json`
      (download size enforced; the two engine-share latencies warn; the rest is #395).
- [x] #172: `tessera-mcp` stdio adapter, compile-only first — `tools/mcp/`, 11 tools with
      three implemented and eight declared-unimplemented, 7 ctest cases; the rest is #396.
- [x] #78: devtools: `#line` mapping to TSX (opt-in, `--line-directives`) and an in-app
      compiler-error overlay. Hot reload is **blocked, not deferred**: ADR-040 emits a
      header-only template, so there is nothing to swap; the seam that must exist first is
      written up in `docs/specs/devtools-protocol-v1.md` section 8.
- [x] #171: P7: studio slices (picker overlay, style inspector, `source.patch`, skins).
      **The picker overlay slice landed** (2026-11-18): `tessera_canvas_pick` answers what is under a
      point — the node's automation id, element kind and logical-pixel box — as a *query* that
      dispatches nothing and owes no frame, reusing `Node::hit_test_deep` rather than a second hit
      test; the page draws the outline and readout in a `pointer-events: none` overlay armed by a
      toggle in the Canvas pane. Evidence: `tools/codegen/scene_canvas_session.*`,
      `tools/codegen/canvas_report.*`, `tools/codegen/wasm_canvas_entry.cpp`,
      `designer/web/src/views/canvas-picker.ts`, ctest `scene_canvas_pick`,
      `docs/design/canvas-input-session.md` §5.1.
      **The skins slice landed** (2026-11-19): `designer-studio-v1.md` §5.2's manifest as data plus
      static art — seven bundled skins under `designer/web/public/skins/` (generic desktop Windows 11 /
      macOS / Linux, generic phone Android / iOS, generic tablet, generic foldable), all art original,
      no vendor device art — validated by `designer/web/src/views/skins.ts` (a typed
      `{ok:false, code:"TESSERA_STUDIO_SKIN"}` result, never a throw; a missing required field, an
      inset larger than the screen, a negative size or a hard-coded colour rejects the skin and it is
      not offered). The geometry contract of §5.1 is one derivation in
      `designer/web/src/views/skin-geometry.ts` (`size - bezel` is the viewer size, and the insets are
      the `safeAreas`), and the insets reach the **engine**, not only the picture: the new
      `tessera_canvas_set_safe_area` export fills `WindowMetrics::insets`/`cutouts` through
      `simulated_metrics()`, so `hal::resolve_safe_area()` pads a `SafeArea` by exactly what the
      manifest declared (`tools/codegen/scene_layout.*`, `scene_session.*`, `scene_canvas_session.*`,
      `wasm_canvas_safe_area.*`). Chrome is page-only HTML/CSS/SVG tinted from the theme tokens, and
      the Theme select now writes `<html data-theme>`, so a theme switch retints the frame and the
      canvas in one cascade (`designer/web/src/regions/SkinFrame.tsx`, `styles/skin.css`,
      `page-skins.ts`). Evidence: `scripts/ci/preview_skins.mjs` (36 checks: manifests, rejections,
      per-category viewer size, the three geometry modes, the retint rule), ctest
      `scene_skin_geometry`, `docs/design/studio-page.md` §1.1.
      **The style inspector slice landed** (2026-11-19): `tessera_canvas_style` answers
      `style.info` (`designer-protocol-v1.md` §9.1) for one node — the computed style, the matched
      rules highest-precedence-first with their origin, selector text, specificity and span, and the
      box — as a *query* that dispatches nothing and owes no frame, like the picker. The provenance
      it needs did not exist: `CompiledRule` now carries `selector`, `offset` and `source_path`,
      threaded from `PreparedItem` at prepare time, and `CompiledStyleSheet::matched_rules()` is the
      one list `add_to_cascade()` consumes, so a rule the inspector names is a rule the cascade used.
      `overridden` is the cascade's own `cascades_over()` answer, never source order. The blob format
      is **unchanged**: the two fields stay out of it, a blob-loaded rule prints its selector back
      (`src/ui/style/selector_text.cpp`) and reports offset 0, and
      `compiled-stylesheet-v1.md` §5.1 records why. The page draws the panel in a page element beside
      the canvas, off by default and armed by an `Inspector` toggle, and it consumes the click it
      inspects (capture-phase `stopPropagation`) so the engine's pointer path never sees it.
      Evidence: `tools/codegen/scene_canvas_inspect.*`, `tools/codegen/canvas_report.*`,
      `tools/codegen/wasm_canvas_entry.cpp`, `designer/web/src/views/canvas-inspector.ts`, ctest
      `scene_canvas_style`, `docs/design/canvas-input-session.md` §5.2.
      **The `source.patch` slice landed** (2026-11-19), completing the issue: `tessera_canvas_patch`
      answers `source.patch` (`designer-protocol-v1.md` §10.2) for one node and destination — the
      minimal `TextEdit` list plus the base versions — as a *query* that writes nothing, because
      applying the edits and answering `source.patch.result` is the studio's project-IO path (§10.3)
      and is **not** wired here. The engine gap it closed: the IR's element `SourceSpan` was dropped
      by the build walk, so nothing could name the bytes a commit edits; it now travels in a
      **side table** (`SceneBuildResult::SceneNodeSource`, owned by `PreviewSession`, replaced on
      every rebuild) rather than as a `ui::Node` field, because `ui::Node` is a public header with an
      enforced `kNodeSizeBudget` and a span is a design-time fact no frame reads. Ranges come from
      the parsers, never from a text search: `inline` re-parses the component and uses
      `JsxAttribute::span`, `class` uses `style::parse_stylesheet`'s `Rule::offset`/
      `Declaration::offset`, `token` scans the theme asset's JSON for the string literal. `edits` is
      sorted by file and descending offset and non-overlapping; the property asserted is that
      applying in the given order equals splicing every range against the original offsets at once
      (not "any order", which is false when replacement lengths differ). Every range is checked
      against UTF-8 code point boundaries. The six spec reasons are exact, and no patch is produced
      for any of them. Narrower than §10.1 and reported as such: the override layer
      (`style.override.set`) is a later slice, so a commit day-one carried no declarations and
      answered `no_overrides`. **#536 landed it** (below). Evidence:
      `tools/codegen/scene_canvas_patch*.{hpp,cpp}`, `tools/codegen/scene_build*.{hpp,cpp}`,
      `tools/codegen/scene_session.*`, `tools/codegen/canvas_report.*`,
      `tools/codegen/wasm_canvas_entry.cpp`, `designer/web/src/views/canvas-inspector.ts`, ctest
      `scene_canvas_patch`, `docs/design/canvas-input-session.md` §5.3.
- [x] #536: studio: the style override layer (`style.override.set`/`clear`/`revert`, undo/redo) so
      `source.patch` is reachable — validated with the stylesheet engine the compiler uses, an
      invalid value answers `ok:false` with its `TESSERA_CSS_*` and leaves the node unchanged, and
      an override changes only that `NodeRef` (never another instance of the same component).
      A `token`-bound override makes a `token` commit succeed instead of `not_token_bound`; a commit
      whose re-evaluated IR disagrees with the committed values reports
      `TESSERA_DESIGNER_PATCH_MISMATCH`. Offsets in `source.patch` are real now (PRs #591 and the
      earlier override landing: `tools/codegen/scene_canvas_override.cpp`, `scene_override.*`,
      `wasm_canvas_override.cpp`, `designer/web/src/views/canvas-overrides.ts`, ctest
      `scene_canvas_override`, `scene_canvas_override_commit`). The page half was wired in PR #591
      (`#585`), which is also where the round trip is exercised
      (`scripts/ci/preview_browser_override_commit.mjs`).
- [x] #395: P6b: the latency budgets are `enforce` with a two-sided ratchet and every module the
      studio loads is weighed. Raw wall clock was not ratchetable (1.7x and 2.7x runner noise across
      eight Pages runs), so the pin is calibration-normalized: each probe times a fixed warmed
      integer workload alongside the compile and reports the ratio (1.04x spread idle and under
      contention, against 1.06x/1.08x raw). Evidence: `scripts/check_preview_budget.py`,
      `tests/golden/perf/preview_budget.json` (PR #438).
- [ ] #396: `tessera-mcp`: the last three unimplemented tools and the #350 spec decisions. Five of
      the eight declared tools have landed — `list_files` (#406), `set_viewport` (with the fold
      posture, #408), `snapshot`, `query_style` and `pick` — all `implemented` in
      `tools/mcp/schema/tools.json`, over the same preview engine the studio uses.
      Remaining: `apply_edits` (deterministic in-memory first, `write: true` behind the shared file
      policy), `screenshot` (needs the native headless renderer / GPU decision, spec open question 4)
      and `run_script` (needs the harness document). The three spec decisions are settled:
      `TESSERA_MCP_*` is a row in `error-model-v1.md` §3, `tools.json` + `toolSurfaceVersion` ship
      (`tools/mcp/schema/tools.json`, reported by `initialize`, gated byte-for-byte by `mcp_tools`),
      and `open_project` reads nothing (section 8). Take `apply_edits` first.
- [x] #406: `tessera-mcp`: project root from the agent's workdir (`roots/list`, else the working
      directory), `list_files` and folder-wide `diagnostics` / `get_ir`. Done: `tools/mcp/src/
      project_files.cpp` and `project_tools.cpp`, the shared file policy `src/schema/src/
      project_files.cpp` (also for the Wasm module), `cli::run` with `Options::query` and `base_dir`;
      tests `mcp_project`, `schema_project_files`; spec `docs/specs/mcp-server-v1.md`. #396 keeps the
      other tools.
- [x] #407: `tessera-mcp`: a `generate` tool for the C++ of the folder (ADR-040). Done: `run_generate_tool`
      in `tools/mcp/src/project_tools.cpp` over `cli::run` with `Options::dry_run` /
      `collect_generated`; test `mcp_project` (dry run, byte-identity with the CLI, developer class
      untouched, outDir and link refusals); spec `mcp-server-v1.md` section 4.
- [x] #408: studio: foldable simulation (hinge, fold posture, two-pane) on the Wasm viewer — the
      keyboard/host safe-area step moved into `ui::style::safe_area_style`, so the preview feeds a
      simulated fold through the *same* path the native app runs rather than a second one
      (`tools/codegen/scene_layout.*`, `tessera_canvas_set_safe_area`); evidence PR #429, ctest
      `scene_skin_geometry`. The hinge **band** as a page overlay is #433; the layout half ships.
- [x] #397: security: nested `:not()`/`:is()`/`:where()`/`:has()` overflowed the stack in the stylesheet
      compiler (found by the #198 review). Fixed: selector nesting is depth-bounded, so #390 and #391
      can feed user CSS to it.
- [x] #398: security: nested `@media`/`@layer`/`@container`/`@supports` overflowed the stack in
      `prepare_stylesheet`. Fixed, independently of #397.
- [x] #399: security: lowering was quadratic in source-line length (28 s for a legal 1 MiB line).
      Fixed: `SourceMap::locate` is logarithmic, so a 1 MiB line no longer freezes the viewer's thread.
- [x] #392: studio: merge the index, scene and canvas pages into one `index.html` (modules stay
      separate and lazy; the `scene.html`/`canvas.html` redirect stubs were later removed, so old
      links no longer resolve). Merged in PR #420; the stack conversion and the P6 budget re-record moved to ADR-054's
      slice (#422), which the studio items below and #391 now wait on.
- [x] #422: studio: convert the hosted page to React 19 + Vite (ADR-054). The four regions are
      components and the engine canvas is a childless ref'd leaf outside React's render cycle, so a
      preview crash loses the canvas and not the editor. `share.js`/`preview-state.js` stay pure ES
      modules (asserted from both sides, and the 19 share checks import them unbuilt). The budget was
      re-recorded from a measured decomposition: React's floor is +69,007 gzip over a control build
      and the page's own JS fell from 44,001 to 13,790, for a net +33,774. **Unblocked #387-#390,
      #395, #408 and #171.** Evidence: `designer/web/src/App.tsx`, `docs/design/studio-page.md`.
      The Pages lane caught a real defect the conversion could not: the sample-manifest generator
      still read the pre-Vite path, so `--check` looked for a file the build had just deleted.
- [x] #387: P3b: text in the canvas preview, font loaded as data — `CountingSink::draw_text` now
      forwards to the production `Batched2DSink` (HarfBuzz shaping + `TextRenderer`'s bounded glyph
      atlas), so the preview and the app cannot drift. The font is a **page asset**, never in the
      `.wasm`: `designer/web/public/fonts/IBMPlexSans-Latin.ttf` (OFL-1.1, with `OFL.txt`), fetched
      and handed in through `tessera_canvas_set_font` (`tools/codegen/wasm_canvas_font.*`,
      `designer/web/src/views/canvas-font.ts`). A failed fetch degrades to the box-only text and a
      named page state. Found by rendering, not by reading: layout and paint had disagreed (the
      stub measurer vs the real one) and both are now the same measurer. Evidence: PR #428,
      `tests/unit/scene_frame_text_tests.cpp`, `scripts/ci/scene_frame_canvas_font_probe.mjs`,
      `docs/specs/dependencies.md` (#280's font pack supersedes the single subset).
- [x] #388: P3c pointer input -- the **engine half** (`CanvasSceneSession` + the
      `tessera_canvas_pointer` export). `:hover`/`:active` come from the real style engine, not from
      JS writing a class: the router sets `Node::pseudo_state` and marks `Dirty::Style`, so
      `Button:hover` matches. One frame per change and **zero for a static scene**, proven over the
      null RHI and mutation-checked (forcing `needs_frame()` true fails both suites). The coordinate
      rule is the CSS-to-logical ratio, **never DPR**. Page wiring is #422's shell, which has landed;
      The page half landed in `designer/web/src/views/canvas-pointer.ts`, verified in a real browser
      (WebGPU, swiftshader): hover, press, click and leave through the style engine, one frame per
      change, none for a static scene. Keyboard focus landed with it (`canvas-keyboard.ts`,
      `tessera_canvas_key`): Tab/Shift+Tab, Enter/Space, `:focus-visible`, no keyboard trap, focus
      kept across the rebuild a click causes; resize and fold keep the app's state. Evidence: `tools/codegen/scene_canvas_session.*`,
      `docs/design/canvas-input-session.md`, `scripts/ci/preview_browser_canvas_pointer.mjs`.
- [x] #389: conditional and repeated children in the scene preview — both build instead of answering
      `TESSERA_SCENE_UNSUPPORTED`. No second evaluator: the condition is evaluated against the one
      `eval::Scope` the scene builder already seeds (carried cells, declared initializers, handler
      assignments), so a branch flips off real state, and a scripted click on `setCount(count + 1)`
      changes the painted branch. Evidence: PR #427, `scripts/ci/scene_snapshot_parity.py` (the two
      dynamic shapes are corpus cases).
- [x] #390: author stylesheet, `style` and `className` in the preview — a sheet is the author's when
      it is not empty and not whitespace/comment-only, and `preview_stylesheet()` is then never
      called; empty or comment-only falls back to the built-in sheet byte-for-byte. A sheet with one
      bad rule still counts as the author's, so an invalid declaration cannot resurrect the built-in
      style. The slice also fixed an unbounded `@supports` recursion #410 missed (20000 nested parens
      aborted the process on `main`). Evidence: PR #458.
- [x] #461: paint: an outer `box-shadow` is clipped to outside its own border box rather than painted
      inside it — `src/ui/paint/paint_shadow.cpp` (PR #488).
- [x] #462: animation: `box-shadow` transitions and keyframes interpolate the shadow list instead of
      snapping to the end value — PR #518.
- [x] #463: style: CSS isolation for components (`*.module.css` scoped class names) — spec, import
      form, scoping scan, lowering, codegen embed and preview landed across PRs #495, #497, #498 and
      #500, the gallery page is the worked example, `tests/unit/theme_scoping_tests.cpp` covers it.
- [x] #457: `background-image: linear-gradient()` from a stylesheet — PR #519.

### V1-M4 Sub-milestone: studio IDE ([#584](https://github.com/thnak/Tessera/issues/584), ADR-060, ADR-062)
**Owner decisions 2026-11-19:** flexlayout-react 0.11.2 behind our own layout JSON and adapter
(dockview fallback); our JSON is canonical; VS Code default chords with browser fallbacks; stable
append-only command ids; layout per viewer only; no floating windows in V1; bottom dock collapsed by
default. Specs: [`studio-workbench-v1.md`](docs/specs/studio-workbench-v1.md),
[`studio-workbench-layout-v1.md`](docs/specs/studio-workbench-layout-v1.md); dock library ADR-062.

Landed in PR #586 (W0-W5): the workbench-core registry (commands, when-clauses, keybinding
resolver), the layout model with panel registry and persistence, the menu bar and toolbar, the
command palette / quick open / context menus, notifications, and the flexlayout dock renderer.
`designer/web/src/workbench/` (panels.json), `designer/web/src/lib/` (commands, layout), the
`workbench_*.mjs` gates.

- [ ] #561: W5b: activity bar and side bar shell (Explorer live; Search and Source Control reserved),
      Ctrl+B and Ctrl+J.
- [ ] #562: W5c: editor groups, preview tabs and path breadcrumbs (may move after V1).
- [ ] #563: W6: rewrite `scripts/ci/preview_browser_layout.mjs` for the dock.
- [ ] #564: W7: keyboard model (F6 region cycle, move-panel commands, separator resize, zen mode,
      layout presets, a11y audit).
- [ ] #565: W8: keybindings JSON and settings UI (Ctrl+,), layout and override import/export.
- [ ] #566: W9: MCP projection of the command registry (`commands-tools.json` generator).
- [ ] #516: dockable preview panes (Scene, Diagnostics, Canvas) in VS Code style.
- [ ] #589: studio: sash snap, double-click reset and collapse below `minSize` (layout spec §4.4) —
      the dock landed with keyboard-operable splitters only; these three are model operations on the
      layout JSON so the keyboard and mouse paths share one code path.
- [ ] #567: `studio-services-v1.md` and a no-UI protocol harness for the service workers (ADR-060,
      **ready**; nothing flips the other worker items until it lands).
- [ ] #569: move the existing compile and diagnostics views behind the worker protocol [ADR-060].
- [ ] #575: Problems panel (`view.problems`, Ctrl+Shift+M) over project-wide diagnostics.
- [ ] #580: cross-file search and replace (`view.search`, Ctrl+Shift+F).
- [ ] #581: asset editor (images, fonts, icons in the project, preview, replace, size and license info).
- [ ] #582: theme and design-token editor (Instrument tokens, light/dark/high-contrast, contrast checks).
- [ ] #583: version history for project files (local snapshots, diff view, restore).
- [ ] #590: retire or rewrite the stale `preview_browser_*` developer scripts — some still target the
      pre-workbench page and are not in CI; keep what `workbench_*.mjs` does not already cover.
- [ ] #391: studio D-S2 + D-S4: open a project folder (File System Access API, zip elsewhere), file
      tree, edit, explicit save, multi-file preview. **Landed** in #508 (open folder/zip, limits, Save
      with conflict check, Download), #511 (new file, rename, delete, Reopen last folder), the
      entry-component dropdown, the bounded Recent-projects list (`designer/web/src/lib/`, one
      handle per directory in IndexedDB), drafts in browser storage, and view-only locked text files
      with a "generated header may be out of date" advisory (PR #586); evidence
      `scripts/ci/preview_project_load.mjs`, `scripts/ci/preview_browser_project_io.mjs`. The issue
      stays open for what is genuinely left: creating an empty directory and one manual OS-picker
      check in Chrome/Edge (folder rename/move/delete landed: `scripts/ci/preview_project_folder_ops.mjs`).
- [x] #431: studio: one three-column workspace (project tree, tabbed code, preview panes for
      scene/canvas/diagnostics) replacing the three top-level tabs (owner direction 2026-11-17).
      Builds on #391's IO layer (zip and path confinement landed in PR #425); keep `?view=` so
      `#p5=` links work. Room must be left for #171's panes and #433's overlay. Landed in #439.
- [x] #433: studio: draw the fold hinge and the safe-area insets as a page-only overlay (the
      follow-up #408 left in `designer-studio-v1.md` section 6.1). Landed 2026-11-27: the Canvas
      pane's **Overlay** toggle shades the hinge band, the safe-area/keyboard/gesture insets and the
      cutout rectangles the running tree was laid out inside. The geometry is the engine's —
      `tessera_canvas_overlay` is a QUERY over `CanvasSceneSession::metrics()`, so arming it presents
      nothing and the band cannot disagree with the layout it explains; the page only scales those
      rectangles (`designer/web/src/views/canvas-fold.ts`, DOM in `canvas-fold-layer.ts`). It is page
      DOM, so never engine paint and never a byte the viewer receives. It refreshes from the APPLY
      that carries a new posture, not from the toolbar callback that precedes it (a refresh there
      reports the device the canvas is about to leave). Evidence: ctest `canvas_overlay`,
      `scripts/ci/preview_fold_overlay.mjs` (scaling, the per-side maximum, the zero-area rules),
      `scripts/ci/preview_browser_fold_overlay.mjs` (wired, one frame per posture, no frame to arm).
- [ ] #690: studio: drag-resize the canvas viewport safely, plus the web-like responsive tools.
      **Slice (a), the limits, landed** (2026-11-27): one clamp holds every size path — a typed size, a
      device profile's DPR, a raw `viewport.set` — to extent 1..8192, DPR 0.01..4 and a 256 MiB
      backing store, so no input can ask for a size the device cannot create. An over-limit value is
      clamped and named (`clamped` in the viewport report, and the pane says which device it got); a
      zero/negative/NaN value is refused and leaves the picture. The rule is one pure function
      (`tools/codegen/canvas_viewport_request.cpp`) with the page's copy in
      `designer/web/src/views/canvas-viewport.ts`, pinned to the same table by ctest
      `canvas_viewport_limit` and `scripts/ci/preview_viewport_limits.mjs`; the slice also fixed
      `tessera_canvas_draw` being handed the logical rectangle instead of the physical extent the page
      sized. Evidence: `docs/design/canvas-input-session.md` §5.4, `designer-protocol-v1.md` §15.3.
      **Slice (b), the drag handles and the coalescing, also landed** (2026-11-27): three grips (right,
      bottom, corner) in the canvas' own positioned box, a live readout, and — the rule the gesture
      exists to keep — **one step per display refresh**, not one per pointer event, plus exactly one
      settled step on pointer-up so the released size is exact even when it lands between two frames.
      The arithmetic is pure and gated in Node (`canvas-resize.ts`, `preview_canvas_resize.mjs`: five
      moves inside one frame cost one step); the DOM half is `canvas-resize-layer.ts`, which **delegates
      from the document in the capture phase** because the session is created before React commits the
      grips, and resolves the readout on first write for the same reason. A drag goes through the
      page's own `setCustomSize`, so the profile chip reads `custom` and every other view reads the
      size the canvas is showing. Two bugs the browser gate caught and the Node gate could not: the
      travel was divided by the **DPR**, which halved every drag at DPR 2 (the canvas' CSS box is the
      *logical* viewport, so the divisor is the pane's zoom and nothing else), and the zoom was read
      from the canvas rather than its zooming stage, where a CSS transform scales the rect and the
      layout box alike and the ratio is always 1. Evidence: `preview_browser_canvas_size.mjs` (the
      grips, the readout, the frame bound, the exact released size — mutation-checked by removing the
      `apply` call), `docs/design/canvas-input-session.md` §5.6.
- [ ] #598: studio: "the canvas stays 320x240 after a viewport change". **Re-checked 2026-11-27 and it
      does not reproduce** against `main`: with the suspected React-owned `width={320} height={240}`
      restored on the element, `canvas.width` still follows the viewport, because the session writes
      after React's commit and React never re-renders that leaf (the issue itself says "strong
      suspect, not proven"). Rather than remove the attributes on a theory they were left alone, and
      the property the issue is about is now a gate:
      `scripts/ci/preview_browser_canvas_size.mjs` drives the Device menu in headless Chromium and
      asserts the store follows the viewport, the CSS box keeps the logical size, and the two agree
      with the reported DPR — at DPR 1 and at DPR 2 (768x1024 → a 1536x2048 store), which is also
      where #599's geometry half lives, since a presented WebGPU frame cannot be read back in
      headless Chromium. The issue stays open for whoever next sees the symptom on a real device.
- [x] #599: studio/engine: at device pixel ratio 2 the app painted at half size in the top-left of
      the canvas — the paint side never applied the device scale, though the layout side was given
      it. `scene_frame_paint_tree()` now composes it onto every position, the border snapping and
      the vector raster extent, exactly as the app's frame does (`src/app/window_frame.cpp`); the
      scale comes from `options.dpi_scale` on the single-frame path and
      `CanvasSceneSession::dpi_scale()` on the canvas path. At 1.0 the transform is skipped, so every
      existing golden is byte-identical. Regression: the parity corpus's `dpr` case (a full-bleed box
      at DPR 2 must show its colour at the far corner of the doubled target — mutation-checked by
      reverting the `set_content_scale` call, which turns it red). Evidence:
      `scripts/ci/scene_frame_parity.py`, `docs/design/canvas-input-session.md` §5.5.
- [x] #436: engine: a window or emulator resize re-resolves `@media` and re-lays out the running
      preview in place — `AppHost::resize` now updates the media context, and `set_viewport` re-lays
      out the live session without re-evaluating the source (state cells, hover and focus survive;
      `cascade_generation()` is the evidence that the cascade re-resolved only when a size crossed a
      breakpoint). Evidence: PR #486, `tests/unit/scene_viewport_tests.cpp`, `src/app/host_viewport.cpp`.
- [x] #440: perf: one state change no longer costs n + 1 `update()` calls — ADR-056 landed in its
      three slices: #441 read-set analysis (`src/compiler/codegen`, `compute_read_sets`),
      #442 the emitted dirty mask (`apply_state` re-evaluates only dirty bindings), #443 a parent
      schedules a child's ticket instead of calling `update()`. Evidence: `codegen_update_fanout`,
      PR #553 (`docs/design/observable.md` §7).
- [x] #444: studio: live edit — an edit applies itself (150 ms debounce, `Ctrl+Enter` applies now,
      last good frame kept on error) and Run became Restart app. `designer/web/src/page-live.ts` is
      the page's only timer; the session's `requestFrame()` collapses a burst of applies into one
      frame, measured via `CanvasSceneSession::frames_painted()`. Evidence: PR #482.
- [x] #445: engine: Back and Volume input events, Home/Recents/Power mappings, unhandled Back and the
      saved-state record (device profiles E1) — `EventType::VolumeKey`, the system-button split from
      text dispatch, `src/hal` mappings. Evidence: PR #553 (`cd66991d`, `8792e706`).
- [x] #446: studio: `platform.event` message and the Wasm viewer path (E2) —
      `tools/codegen/wasm_canvas_platform.cpp`, `tessera_canvas_platform_event`, the shared
      `PlatformEventDriver` (`scene_platform.cpp`) both the Wasm export and the native scripted run
      use, `designer/web/src/views/canvas-platform.ts`, `designer/web/src/lib/platform-events.ts`;
      `scene_input_parity.py` gained five platform cases. Evidence: PR #586.
- [x] #447: studio: device profiles as data, selector, restart on device change (E3) — seven profiles
      under `designer/web/public/profiles/`, `lib/device-profile.ts` + `device-profile-load.ts`.
- [x] #448: studio: platform panel, hardware button strip, ordered event log and replay (E4) —
      `designer/web/src/workbench/PlatformDock.tsx`, `designer/web/src/page-platform.ts`,
      `scripts/ci/preview_platform_events.mjs`. Evidence for #447/#448: PR #586.
- [x] #434: studio: mock services (`*.mock.ts`), console and call trace — the **studio-side half**
      landed in PR #586: the `mock` accepted-file kind (64 files / 8 MiB), the static subset scan,
      the sandbox worker with network/clock/DOM/dynamic-code globals removed, `designer/web/mock/`,
      the Mock console and Call trace dock (`mock-console.ts`, `MockConsolePanel.tsx`), the lazy
      esbuild-wasm transpiler with its own budget row, `preview_mock_host.mjs` / `preview_mock_sandbox.mjs`.
      The engine-visible half (mocks feeding the preview) stays gated on the ADR-050 service seam,
      which is why `studio-mock-services-v1.md` is still "proposed"; that residual is tracked under
      #642, not here.
- [ ] #435: studio D-S9: startup-phase preview (freeze at P0, P2, P4). **Blocked** on startup slices
      S3 and S5 in Wasm; one open question first (real `AppHost` or reduced stage machine).

### V1-M5 Application framework ([milestone 5](https://github.com/thnak/Tessera/milestone/5))
ADR-050 is Accepted (roadmap AF1-AF8); start with AF1.

- [x] #173: AF1: `ServiceContext` + `Service` state machine, ordered async start/stop, deadlines — new
      `tessera_appfw_core`, tests under `ManualClock`/`ManualExecutor`.
- [x] #174: AF2: `TaskScope` + `RuntimeLoop` timer wake source — timers in order; idle parks. Evidence:
      `include/tessera/appfw/task_scope.hpp` (`spawn`/`cancel`/`drained`/deadline-bounded `close()`,
      bounded children) and `include/tessera/runtime/timer_queue.hpp` +
      `RuntimeLoop::next_timer_wait_seconds()` in layer 0 with no `appfw` name in it; ctest
      `appfw_task_scope`, `runtime_timer`; `docs/design/background-work.md`.
- [x] #175: AF3: `Application` lifecycle; `WindowHost` forwards focus/minimize/occlusion. The
      dispatch order is asserted as an explicit vector (forward in start order, stop in reverse),
      `src/app/window_lifecycle_seam.cpp` forwards the existing fold rather than adding a second
      one, and `tests/fixtures/lifecycle/desktop.json` is the per-platform script.
- [x] #176: AF4: `Observable<T>` over `UpdateScheduler` — one flush per tick, no allocation on
      `set`: `include/tessera/appfw/observable.hpp`, `docs/design/observable.md`, tests
      `appfw_observable` (`tests/unit/appfw_observable_tests.cpp`,
      `appfw_observable_allocation.cpp`).
- [x] #177: AF5: `useService<T>()` codegen -- an undeclared service is a compile error with
      a stable code, `TESSERA_CG_SERVICE_UNDECLARED`, naming the component, the service type
      and every declared requirement with its TSX `file:line:column`. Resolution is
      structurally incapable of running in the frame path: the context is not retained and
      the accessors read pre-resolved pointers (1000 reads inside a `FrameScope` ->
      `in_frame == 0`). Evidence: `src/compiler/codegen/src/emit_services.cpp`, ctest
      `codegen_services_generate`/`codegen_services_runtime`, `docs/design/services/
      service-injection.md` section 11.5.
      TSX half landed (#925): `ui/services.json` reader, hook rules, `callService` IR action,
      `codegen_services_tsx_runtime`. **Still open:** `services.d.ts`, startup validation,
      manifest delivery to Studio/preview/mcp (section 11.6).
- [x] #178: AF6: `NativeSurface` refs + `SurfaceRenderer` -- the surface is a **borrowed**
      ref-counted view over handles the HAL owns (no deleter, no teardown hook), proved in
      both directions against a window that counts its own teardowns. The golden
      (`tests/golden/baseline/surface/native_surface.png`) was written on a device whose CPU
      class is read from `VkPhysicalDeviceProperties`, not from `VK_ICD_FILENAMES`, and is
      **bit-identical** between the local llvmpipe and the remote lavapipe lane
      (`237bbaeb...`). Evidence: `include/tessera/hal/surface.hpp`,
      `tests/gpu/surfaces/`, `docs/design/native-render-surfaces.md` section 10.4.
- [x] #179: AF7: `storage` module, atomic writes on `IoExecutor` — corruption fallback
      tested. Same-directory temp + flush + atomic replace
      (`src/appfw/storage_atomic.cpp`), the `IoTaskExecutor` seam in layer 0, and a fake
      `FileSystem` that fails at one named step so atomicity is proved rather than hoped for.
- [x] #180: Multi-window support: two windows with independent lifecycle, damage and pacing, and a
      static window that still idles with zero presents. Layer 1 owns the aggregate:
      `appfw::WindowRegistry` (`include/tessera/appfw/window_registry.hpp`) is a fixed-capacity table
      of `WindowLifecycle` values plus `aggregate_window_lifecycle`, the pure function that folds
      them into the one aggregate `derive_app_state` already consumed - ADR-050 section 2 derives
      every state from a phrase about the *set* ("a window focused", "all windows blurred", "no
      visible window"), so AF3's single member was a one-window special case of the rule. The
      primary window is registered by `Application`'s constructor, so a single-window app's
      aggregate is exactly the member AF3 had and every pre-#180 caller is unchanged;
      `attach_window()` adds the others and returns a `WindowId` that is never reused, and
      `make_lifecycle_sink(WindowId)` gives each window its own seam. Two rules the aggregation
      adds: a closed window is not visible even when its stale flags say it is focused, and closing
      one of two windows is not a termination request (`quitOnLastWindowClosed` is about the last
      *open* window). Layer 0 owns the walk: `WindowDevice` keeps the live window list and
      `pump_windows_once()` gives every window one bounded iteration. Two per-tick resources had to
      move to the group for that to be correct, and the two-window pacing case found both:
      `RuntimeLoop::tick()` *consumes* the render request, so a second window that ticked the loop
      itself never painted; and the app-scoped pacer gates *ticks*, so advancing it per window let
      the first window in the walk starve its siblings. Neither is visible without a real cap (an
      uncapped pacer always says "present now"), so the test asks for 30 Hz explicitly. Evidence:
      `tests/unit/appfw_window_registry_tests.cpp` (ctest `appfw_lifecycle`: the aggregation table
      row by row, close-one-of-two, the tray-app row, per-window sinks, the capacity bound),
      `tests/gpu/app_window_multi_loop_tests.cpp` (ctest `app_window_multi_loop`: one loop drives
      both, independent damage, both present in one group tick, a settled window presents nothing,
      the single-window path unchanged), and `docs/design/multi-window.md` section 10.
- [x] #181: Tray apps: a tray app runs with no visible window and zero presents, and shows a window
      on activation. The lifecycle half was already right after #180 - an empty window set is
      `closed`, which `derive_app_state` maps to `Hidden`, and `quitOnLastWindowClosed = false`
      keeps the app running - so this slice is the layer-0 half, and it is two seams. Hiding is the
      *app's* decision, not a platform query: `WindowHost::set_visible(false)` stops the driver
      presenting and `set_visible(true)` shows it and latches a full repaint, tracked as a driver
      flag rather than through `WindowPausePolicy`, because a `pauseWhenOccluded=false` config must
      not be able to make a hidden window paint. That is deliberately different from
      `WindowOptions::visible = false`, which is the offscreen/CI pattern where the app paints into
      a window nobody sees and must keep presenting - conflating the two hangs every bounded
      `run(N)` on an offscreen window, which is what the first version of this slice did to twelve
      GPU tests and two samples. The loop half is `WindowDevice::run_windows()`: the only loop
      before it was `WindowHost::run()`, which parks on *its own* window's event wait, so an app
      with no window had no loop at all. `next_wait_seconds()` is the nearer of the pacer deadline
      and the next timer, clamped so a window-less park is bounded, and `request_stop()` is the only
      way such a run can end (consumed, not merely observed, so a second run is not stopped by the
      first one's request). Evidence: `tests/gpu/app_window_tray_tests.cpp` (ctest
      `app_window_tray`: a hidden window presents nothing across several iterations and after
      damage, an offscreen-created window still presents, showing paints and the first frame is a
      full repaint, hiding again stops presenting and keeps the window in the group, a window-less
      group reports a wait and a bounded run returns at once, `request_stop()` ends an unbounded
      run, and a tray app activates into a window and paints it - verified to fail with the fix
      reverted), the layer-1 half in `tests/unit/appfw_window_registry_tests.cpp` from #180, and
      `docs/design/tray-apps.md`.
- [x] #228: One `Application` loop and one `rhi::Device` for several windows (before AF6). The
      shared owner is `app::WindowDevice` (`include/tessera/app/window_device.hpp`,
      `src/app/window_device.cpp`): one `rhi::Device`, one `Batched2DRenderer`, one `TextStack`
      (face, rasterizer, shaper, glyph atlas), one pipeline set, one `RuntimeLoop` and one
      `FramePacer`, with each `WindowHost` owning only its surface, swapchain, `AppHost`, input pump
      and accessibility host. The rule that decides every row is "created against the device or the
      swapchain format -> shared; created against a surface or a scene -> per window". The one number
      the refactor changes is the renderer's frame ring: `frames_in_flight * window_count`, because
      one tick can call `begin()` once per window. Device loss is serviced once by the group and fans
      out to every window, and a window whose swapchain cannot be rebuilt reports `stalled` without
      taking its siblings down. The single-window `WindowHost` constructor builds a private
      one-window group, so every existing caller and test is unchanged. Evidence:
      `tests/gpu/app_window_multi_tests.cpp` (ctest `app_window_multi`: one device for both windows,
      closing one leaves the other presenting, a resize of one does not relayout the other, a second
      window does not reset the group's pacing), `docs/design/multi-window.md`, and the five specs
      the issue named reconciled (`custom-elements-v1` Q9, `runtime-config-v1` Q5,
      `resource-lifecycle-v1` Q7, `overlays-and-layers-v1`, `startup-api-v1` section 10).
      **Still open:** the `Application`-level window registry (that is #180's slice), per-window
      pacing on a different-refresh monitor, and window groups with independent devices.
- [x] #450: Platform shell implementation behind `ShellService`: the module (`tessera_appfw_shell`)
      owns the policy - capability negotiation with `Unsupported`/`Denied` kept distinct, the
      clipboard bound, the secure-field rule, one operation at a time, capability handles validated
      as a token/path pair, the section 8 path rule (traversal, drive-relative, executable) and the
      drop budget - and the Win32 back end (`tessera_appfw_shell_win32`) owns the platform: clipboard
      through `hal::Clipboard`, `IFileOpenDialog`/`IFileSaveDialog`/`FOS_PICKFOLDERS` on an
      `IoTaskExecutor` worker, and `WM_DROPFILES` for external file drop. Evidence:
      `include/tessera/appfw/shell{,_types,_backend,_path,_fake}.hpp`, `src/appfw/shell*.cpp`,
      `src/platform/windows/shell_win32*.cpp`, ctest `appfw_shell` (six TUs, deterministic under
      `ManualClock`/`FakeShell`), `docs/design/shell-module.md`. **Still open:** the Linux/X11,
      macOS and mobile back ends, clipboard files/URIs, OLE drag with a drag image, the menu bar and
      tray models, and the `useClipboard` hook - all recorded in section 7 of the design note.
- [x] #451: Permissions: `permissions-v1` spec and `PermissionService` (spec first; #452 and #455 need it).
      **Done.** `docs/specs/permissions-v1.md` owns what five other documents mentioned but none
      defined: the five-state model (`NotDetermined`/`Granted`/`Denied`/`PermanentlyDenied`/
      `Restricted`), the closed V1 permission set with its per-platform mapping, one `request` and one
      `status` per permission with app-supplied rationale text, and the `tessera.json` declaration.
      `tessera_appfw_permission` (`include/tessera/appfw/permission.hpp`, `permission_service.hpp`,
      `permission_fake.hpp`) implements it: the four "do not ask" rules (granted, permanently denied,
      restricted, and an opted-out denial all resolve from the state table without a prompt),
      coalescing of a request that arrives while one is pending (one prompt, one answer, N callers),
      the `NotDeclared` check before the platform sees a request, and per-permission state updated
      only by a result. `FakePermissionBackend` scripts every state, a promptless platform row, a
      platform failure and a prompt that never answers. `tests/unit/permissions_tests.cpp` (22 cases,
      `appfw_permission`) covers each transition, coalescing, cancellation, staleness, eviction and the
      name round-trip; each was proven to fail with its rule disabled. `tessera.json` `permissions`
      parses and validates in `tools/codegen/project_identity.cpp` (unknown id names the valid set,
      duplicate is a no-op, non-array rejected), covered by `codegen_cli`.
      **Still open:** no per-platform back end ships (the fake is what an app, a headless run and the
      studio panel use); the `TESSERA_PERM_UNDECLARED` front-end diagnostic is specified in section 7
      rule 2 but the AST check lands with #455, which is the first component that requests a
      permission; permission groups, iOS provisional authorization and the `permissionText` manifest
      emission are open questions 1-3 of the spec.
- [ ] #452: Notifications: `notifications-v1` spec and `NotificationService` (spec first; after #451).
- [ ] #453: Background work policy: design note, in-process `TaskScope` or OS-scheduled jobs (decision, no code).
- [x] #454: App-level test support `tessera_testing` and a guide (`ready`): service fakes, lifecycle injection, `tap/find/advance`.
      **Done.** `include/tessera/testing/` (`test_app.hpp`, `expect.hpp`) and the `src/testing/` target:
      one deterministic stack (`ManualClock`, `CompletionQueue`, `ManualIoExecutor`, `TimerQueue`,
      `RuntimeLoop`, `AppHost`, `InputPump`) with the verbs an app test wants (`tap`, `type_text`,
      `send`, `press`, `advance`, `pump_ticks`, `find`, `text_of`, `snapshot`, `expect_text`). The
      pump gets the host as its input filter, as the window driver does, so `BackRequested` reaches the
      app's handler instead of the scene (which has no Back event). `provide<T>()` registers in the
      `ServiceContext` *and* the runner, because getting those out of step is the wiring mistake the
      target exists to prevent. The AF7 fake is promoted to `include/tessera/appfw/filesystem_fake.hpp`
      beside `net_fake`/`shell_fake` so an app test and the storage contract suite assert against one
      definition, and it now treats a self-replace as a no-op rather than deleting the file.
      `docs/guides/testing-your-app.md` is the worked example on the Counter fixture. Tests:
      `testing_acceptance` (a tap increments, a conditional child appears, typing reaches the
      generated `onChange`, a failed `FakeNet` shows the error state, Back pops a route and is then
      reported unhandled) and `testing_kit` (determinism across two runs, `advance` arithmetic, a
      missing locator, duplicate ids, the file-system fake, idle paint counts). No shipping target
      links `tessera_testing`, and it links no window or GPU backend, so the suite runs on every lane.
      **Still open:** #451/#452 add the permission and notification fakes, and the form sample's test
      lands with the forms lane.
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
- [x] #280: Library stage 0: IBM Plex bundled font pack, exact theme pin of the pack, token swatch goldens
      (evidence: `packages/tessera-fonts/ibm-plex/` + `font-pack.json` hashes, `tests/unit/font_pack_tests.cpp`,
      `docs/specs/theme-versions-v1.md` section 5, lavapipe goldens `tests/golden/gallery/swatch_*.png`
      via `gallery_golden_swatch_*`). Not done: the pin check is not wired into `tools/codegen`; `tnum`
      is not applied by the shaper (#966, Plex figures are tabular by default).
- [ ] #286: Callback props on user components. Step 1 (forwarding `onClick={onPress}`) done: `src/compiler/lowering/src/callback_forward.cpp`, IR `callProp`, codegen `emit_call_prop`, tests `lowering_callback_forward`, `codegen_callback_forward_runtime`, spec `jsx-subset-v1.md` section 9.5. Step 2 (calling a callback from a handler body, `callback_forward.cpp` + `handler_actions.cpp` `check_callback_calls`) done in the stacked PR. Step 3 (performance gate: zero allocations per forwarded event, depth bounded by IR cycle check, `dispatch_event_to` no longer allocates its path) done in its own PR.
- [x] #238: Library stage 0: `tessera icons add` (Iconify import tool with the license gate) —
      `tools/icons/` (`tessera_icons_core` + the CLI), license gate per ADR-047 item 8 with a unit
      test per class (`icons_license`, `icons_gate`, `icons_cli`), pinned fixtures
      `tests/fixtures/icons/`, byte-exact golden `tests/golden/icons/{basic,monochrome}` with
      determinism + tree-shaking (`icons_import`), `THIRD_PARTY_NOTICES.md` icon-set policy,
      `docs/design/component-library.md` §3.1 status.
- [x] #239: Library stage 0 exit: a styled Button end to end and the gallery skeleton (native lane; the web lane is #290). Evidence: `packages/tessera-ui/Button.tsx`, `samples/gallery`, `tests/golden/gallery`, ctest `library_button_runtime` and `gallery_golden_*`.
- [ ] #240: Library stage 1 tracking (ready; #223 `SafeArea` is its first item).
  - [ ] #530: `Icon` over the emitted icon set (partial: component, tool glyph table, tint, gallery page and
    goldens; Chrome parity page open) — `docs/specs/components/Icon.md`, `tests/golden/gallery/icons_*.png`.
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
- [x] #183: Retained/immediate hybrid chart nodes with damage rects; frame-budget metrics in the overlay.
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
- [x] #187: Reduced motion and dark mode queries on Linux — the HAL reports both normalized
      preferences and a change arrives as an event. `include/tessera/hal/appearance.hpp` is the
      normalized snapshot (`ColorSchemePreference`, `reduced_motion`, `assumed` bits per ADR-023
      rule 7, the documented light/motion-on fallback and the packed handoff),
      `include/tessera/hal/portal_appearance.hpp` + `src/hal/window/portal_appearance.cpp` are the
      pure freedesktop-portal decode (`org.freedesktop.appearance` `color-scheme`/`enable-animations`
      with the animations->reduced-motion inversion, the `Read`/`SettingChanged` decoders and the GTK
      `settings.ini` fallback), and `Window::appearance()` is the platform-neutral read. The Linux
      source is `src/platform/linux/appearance_portal.{hpp,cpp}` (one worker thread, one D-Bus
      connection through the in-tree client, `SettingChanged` subscription; the UI thread only does
      an atomic load, so nothing blocks the frame path) and the Windows source is
      `src/platform/windows/window_appearance_win.cpp` (`SPI_GETCLIENTAREAANIMATION` +
      `AppsUseLightTheme`), both delivered as the existing normalized
      `SystemSettingsChanged`/new `ThemeChanged` event; `AppHost::set_appearance` applies the motion
      flag and re-resolves the prepared stylesheet (`ui::style::with_appearance`,
      `AppHost::media_context`), and `WindowHost::apply_appearance` is the tick-boundary re-read.
      Evidence: `hal_appearance` (`tests/unit/hal_appearance_tests.cpp`), `app_host_appearance`
      (`tests/unit/app_host_appearance_tests.cpp`), `appearance_portal`
      (`tests/unit/harness/appearance_portal_tests.cpp`), `os_event` (`ThemeChanged` mapping),
      `a11y_dbus_*`/`a11y_atspi_*` unchanged in what they assert. Design in
      [`docs/design/hal.md`](docs/design/hal.md) §1.7 and
      [`docs/specs/os-events-v1.md`](docs/specs/os-events-v1.md) §5/§5.1; the D-Bus client moved to
      the bottom-level `tessera_dbus` target (`src/dbus`, `include/tessera/dbus`) so the HAL can use
      it without depending on `ui/accessibility`. Not exercised: a live portal on a real desktop and
      a Wayland-only session (the Wayland backend has no appearance source yet); the loopback/decoder
      tests are the deterministic evidence.
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
- [ ] #455: Media capture: `media-capture-v1` spec (camera, microphone, recording) with a fake device; after #451.

### V1-M8 Release readiness ([milestone 8](https://github.com/thnak/Tessera/milestone/8))
Acceptance: a real app ships and passes validation layers and golden tests in CI.

- [x] #192: Crash capture, symbolication and an opt-in uploader. A forced crash in a
      **child process** writes a report (`tessera.crash.v1`, `reason=access_violation`,
      `code=3221225477`), and symbolication resolves against a real 3,035-symbol `/MAP`
      and a real `.symtab`. The uploader is off by default, proven by four paths asserting
      `call_count() == 0`, with a planted-marker redaction test. Evidence:
      `include/tessera/diagnostics/crash_*.hpp`, `tests/unit/crash_{capture,symbols,
      upload}_tests.cpp`.
      **Open finding:** a Release Windows build emits **no PDB and no `/MAP`** (verified: 576
      executables, 26 PDBs all from `_deps`, 0 maps), so a release crash report has addresses
      and no names until the packaging lane ships one. Minidumps need an out-of-process
      handler (Crashpad/WER), which is a `dependencies.md` decision.
- [x] #193: Updates and distribution. Channel data, an append-only release ledger, derived
      and committed wire manifests, and the update-client **policy** seam. Two `project` runs
      are byte-identical, the gate refuses a signature with no pinned key, and `plan_update`
      refuses an unsigned package rather than faking verification. Evidence:
      `release/{channels,ledger}.json`, `scripts/{gen,check}_release_manifest.py`,
      `docs/guides/packaging.md` section 15.
      **Not landed, each needing a decision:** signing (an Ed25519 key and a CI secret), the
      update host, the downloader (no test may open a socket), staging/rollback-apply, and
      deltas (the spec defers them to v1.1).
- [x] #194: Versioning policy for RHI/IR/project config; public API listing gate (#76).
      Two gates, deliberately not one: `check_public_api.py` records declaration-level surface
      (2,275 lines / 321 files) and **refuses removals**; `check_version_policy.py` fails an
      unversioned breaking change to the RHI headers, IR schema or `tessera.json`. The second is
      not a duplicate: #76 is blind to enumerators, class members, defaults and config schemas,
      which is most of what breaks. `docs/specs/versioning-v1.md` section 11 is the policy.
- [x] #195: i18n app-framework module. `LocaleTable::resolve` over ordered requests, a compiled
      `MessageCatalog` with a fallback chain, and a `LocalizedText` binding whose dirty mask is
      **computed**: a wider translation raises `Layout | Paint | Render`, a same-width one only
      `Paint | Render`, a direction flip adds `Style`. Evidence:
      `include/tessera/appfw/i18n_*.hpp`, `tests/unit/appfw_i18n_*`, `docs/design/services/i18n.md`
      section 2.1. ICU MessageFormat, number/date formatting and the catalog generator are the
      next slice, because they need the ICU data pack that CI does not enable.
- [x] #199: release CI lanes run the validation-layer and golden suites against a declared
      lane/suite matrix (15 suites on linux, 6 on windows, and 10 declared unable to run with the
      reason). The gate also found that `batched2d_vulkan_validation` **passed while warning that
      the validation layer was unavailable**, so a lane without the layer was green having
      validated nothing; that text is now forbidden. Evidence:
      `scripts/check_release_suites.py`, `.github/workflows/{linux,windows}.yml`,
      `docs/guides/packaging.md`.
- [x] #77: `tessera new` scaffolds a project the codegen CLI accepts — 12 files, two templates,
      byte-identical on a rerun with the same name. `generate`/`build`/`run` and the prebuilt
      toolchain are deliberately out of scope: the last needs a release archive and a CI decision.
      Evidence: `tools/codegen/new_project*.cpp`, `src/harness/cli.cpp`.
- [x] #196: navigation app-framework module. Route/back/deep-link/restore, each with a
      verbatim assertion: an unknown route is `NotFound` and records nothing, an empty-stack
      back is `Empty` and never underflows, a cold deep link seeds the root then pushes
      (`Synthesized`), and restore round-trips byte for byte through AF7's `Storage`.
      Evidence: `include/tessera/appfw/navigation_*.hpp`, ctest `appfw_navigation`,
      `docs/design/services/navigation.md` section 3.1.
- [x] #197: networking app-framework module (ADR-052). The state machine is a value type
      asserted directly, and the outcomes are **named** rather than collapsed: a deadline is
      `Timeout`, a cancel is `Cancelled`, a TLS failure is `Transport`, a refused connection
      is `Connect` (not `Offline`), and a 4xx/5xx is `HttpStatus`, not an error. No implicit
      retry, because the spec forbids one, so a deadline is asserted to be a *timer* rather
      than a sleep. Evidence: `include/tessera/appfw/net_*.hpp`, ctest `appfw_net`,
      `docs/design/networking-module.md`.
      **Back ends are one PR each** (WinHTTP, libcurl, `fetch`, NSURLSession, Android), and
      each must be configured to follow **no** redirects, since the module does.
- [x] #198: security review -- `docs/specs/security-review-v1.md`: trust boundaries over TSX,
      stylesheets, `tessera.json`, deep links, network responses, update manifests and crash
      reports, with **ten findings filed** (#397-#405, #409), each probed and carrying a
      `file:line`. The headline finding is that **the compiler's escaping held but its resource
      bounds did not**: a 7 KB imported stylesheet aborted `tessera_codegen` with `0xC00000FD`.
      Both stack overflows (S1 selector recursion, S2 nested at-rules) are **fixed** in a
      follow-up PR with regression tests that assert the call returns at all.
      **Remediation: 9 of 10 findings fixed** -- #397/#398 (#410), #399 (#413), #400 (#417),
      #401 (#416), #402 (#415), #403 (#414), #404 (#419), #409 (#418). Only #405 remains, and
      it needs a spec decision rather than a patch: `ui.limits.*` is documented as configurable
      in `project-config-v1.md` but nothing reads it, so either the keys get wired to the
      existing `TESSERA_*` limits or they come out of the schema.
- [x] #13: memory budgets in CI. 11 rows enforce at 0% tolerance (`sizeof_node` 312,
      `sizeof_input_event` 344, `sizeof_paint_data` 512, `sizeof_node_id` 8, five allocation
      counts) because the probe runs twice and fails if the two disagree; 2 RSS rows warn,
      because the two lanes differ 11% with an identical `sizeof(Node)`, so RSS cannot be a
      shared enforced number. `--update` refuses to loosen. Android PSS and VMA are
      **declared and deliberately unmeasured**, and the validator rejects a budget on them.
      Evidence: `scripts/check_memory_budget.py`, `tests/golden/perf/memory_budget.json`.

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
| Resources / services / security | `docs/design/resources.md`, `docs/design/services/`, `security.md` |
| Testing / goldens | [`docs/guides/testing.md`](docs/guides/testing.md), [`docs/specs/golden-tests-v1.md`](docs/specs/golden-tests-v1.md) |
| Build / packaging | [`docs/design/build-and-packaging.md`](docs/design/build-and-packaging.md), [`docs/guides/packaging.md`](docs/guides/packaging.md) |
| Glossary | [`docs/reference/glossary.md`](docs/reference/glossary.md) |
| Batched2D demo | `tools/poc/batched2d/` |
| Compiler frontend/IR/codegen | `src/compiler/{analyzer,rules,ir,codegen}/` |
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
      `scripts/check_source_size.py` enforces it (gate `source-size`): a new over-limit file, or any
      file that grows past its count in `scripts/source-size-baseline.txt`, fails; the baseline only
      accepts a lower number (`--update` refuses to raise one).
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

**Farm order:** V1-M1, V1-M2 and V1-M3 are complete. V1-M2 closed with the ADR-040 one-path
migration (#218 migrated every test family, sample and tool to the generated base; #219 deleted the
legacy output, the merger and `EmitMode`), the documentation slice #211, and #158 (object-literal
prop defaults and `useState` record initials, ADR-051); its only open item is #327's
`emit_expr.cpp` / `emit_node.cpp` split, whose gate landed in #340. #463 (CSS isolation) landed
across #495, #497, #498 and #500, with the gallery form the worked example and the authored-class
lookup and isolation tests closing it. **V1-M4 is the
active milestone**: every P0-P7 stage (the diagnostics page at
https://thnak.github.io/Tessera/, the null-RHI evaluator, the WebGPU canvas, input, share/gallery,
budgets, the MCP adapter and the devtools overlay) has landed, as have the studio shell (ADR-054,
#422), the folder IDE's IO layer (#391, mostly), live edit (#444), device profiles (#445-#448), the
workbench (W0-W5, PR #586) and the studio-side mock services (#434). What is left is the IDE work in
#584 (W5b-W9, #516, #567-#583, #589, #590), the canvas viewport work (#690), the fold-hinge overlay
(#433) and the last three MCP tools (#396). V1-M3 closed with #159: the
`shader-manifest-v1.md` id-to-source table (section 3.2) and the opt-in cross-compile
section (5.1), plus the `shader-cross` job in `.github/workflows/linux.yml` that runs
`TESSERA_SHADER_CROSS=ON` — its first run found a real shader defect (a by-value struct parameter
in `batched2d.frag` lowered to `OpCopyLogical`, which naga rejects on Linux glslang 16.1), now
fixed. V1-M3 is therefore complete (9 of 9 items; #245 closed too). V1-M4 is in progress,
not blocked on tooling: the WebGPU lane installs the pinned Emscripten and builds Wasm green, and
the preview runs in a real browser. V1-M5 is unblocked (ADR-050 is Accepted) and
AF1 (#173) may start now. V1-M6, V1-M7 (#187 now has the #156 display/event seam it needed) and V1-M8
come later.

**Studio front end (ADR-054, accepted 2026-11-16).** The hosted studio's chrome is React 19 +
TypeScript built by Vite over the static Wasm host page; React renders the chrome only and the engine
still draws just the preview canvas. The conversion landed in #422 (PR #426) over #392's merged
three-view shell (PR #420), and everything after — #391, #171's panes, #408, #387-#390, the
workbench — builds on it. Design-time mock services are `*.mock.ts` project files run in a sandboxed
studio worker and never in the engine ([ADR-055](decisions/ADR-055-studio-mock-services.md)); the
studio-side half ships, the engine-visible half waits on the ADR-050 service seam. Both are
design-time only: no JS/React runtime enters a production app binary ([ADR-024](decisions/ADR-024-native-first-boundaries.md),
non-negotiable 7), and Node/npm stays out of the native build.

**V1 gaps added 2026-11-13 (ready now):** #226, #227, #229, #230, #231 and #235 (front end, style and
paint gaps that make V1 look unfinished), #228 (multi-window refactor, before AF6), #232, #233,
#222, and the library stage 0 items #234, #237 and #238. #226, #229 and #231 touch the IR and the
front end, so land them one at a time. Stage 0 is closed (#239), so #236, #240 and #223 are unblocked.
`src/compiler/codegen/CMakeLists.txt` carries the goldens and fixture wiring of every migrated
codegen family, so a lane adding a family edits it alone.

The detailed history of what landed and why is in
[`docs/architecture/implementation-log.md`](docs/architecture/implementation-log.md). Keep the B1
RHI, batched 2D renderer and scene graph stable.
