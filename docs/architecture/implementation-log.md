# Implementation log

Historical status narrative moved verbatim out of [AGENTS.md](../../AGENTS.md) on 2026-11-12, when
the roadmap was mapped to the V1 milestones (only link targets were rewritten to stay relative to
this folder). It records what landed in Big Steps B2, B3 and C up to commit `571a534` and is **not**
a worklist: several "Next" notes below were done after they were written. The worklist is
[AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step); check the code before
trusting a sentence here. Paths in backticks are relative to the repository root.

## Current focus (as of 2026-11-12)

**Big Goal: B2 runtime foundation.** Implemented: deterministic fixed-step accumulator
(`src/runtime/fixed_step.*`), clock interface + deterministic `ManualClock`
(`src/runtime/clock.*`), `RuntimeLoop` render-on-demand/wake state
(`src/runtime/runtime_loop.*`), `FramePacer` deadline scheduling
(`src/runtime/frame_pacer.*`), normalized input event + bounded queue/coalescing
(`src/ui/input/`), pointer input -> scene hit-test/dispatch + pointer capture
(`src/ui/scene/input_dispatch.cpp`), scene dirty-domain flags/ancestor aggregation
(`include/tessera/ui/dirty.hpp`, `src/ui/scene/node.cpp`), and pruned dirty collect/clear
(`src/ui/scene/dirty_pass.cpp`), and damage accumulation
(`src/rendering/damage/damage.cpp`), CSS tokenization (`src/ui/style/tokenizer.cpp`), and the
tolerant stylesheet AST parser (`src/ui/style/parser.cpp`), selector parsing/specificity
(`src/ui/style/selector.cpp`), right-to-left matching/indexing
(`src/ui/style/selector_match.cpp`), cascade/computed-style/inheritance
(`src/ui/style/cascade.cpp`), and the top-level stylesheet pipeline
(`src/ui/style/compiled_stylesheet.cpp`), the property registry
(`src/ui/style/property_registry.cpp`), media query evaluation
(`src/ui/style/media_query.cpp`), and keyframes parsing (`src/ui/style/keyframes.cpp`). `@media`
nested rules, `@keyframes`, `@layer` precedence, and the `Theme` origin are wired into the
compiled stylesheet. The Yoga Flexbox adapter is
`include/tessera/ui/layout/layout_engine.hpp` + `src/ui/layout/layout_engine.cpp` with vendored Yoga
v3.2.2 (`third_party/yoga`), mapping computed-style longhands to fractional absolute boxes. Text
measurement is wired through `ui/text/text_measure.hpp` and a Yoga measure callback, with a
deterministic stub until the real text stack lands; `ui/text/font_metrics.hpp` supplies
ascent/descent/line-gap for baselines and normal line height; `ui/text/text_layout.hpp` exposes the
wrapped line boxes from the same pass. `apply_layout` writes absolute boxes back to
scene nodes as parent-relative bounds with dirty propagation, and `ui/layout/pixel_snap.hpp` holds
the paint-time edge-snapping contract (fractional layout stays unchanged). `add_scene_damage` turns
dirty Paint/Render scene bounds into snapped damage rectangles, and `DamageTracker` keeps prior
bounds to damage both old and new positions. `rendering/redraw_plan.hpp` converts damage rects into
integer `rhi::Scissor` regions with a full-redraw fallback and an area/scissor-count policy for
choosing partial vs full redraw. `@supports` evaluates
`not`/`and`/`or` declarations against the property registry, and `ui/style/color.hpp` parses and
interpolates CSS colors (named/hex/rgb/rgba/hsl). `ui/animation/keyframes.hpp` samples
parsed keyframes at fixed-step time with direction/fill and linear numeric interpolation, and
`ui/animation/animation_shorthand.hpp` parses `animation` shorthands and longhand lists,
`ui/animation/animation_start.hpp` looks up keyframes and batch-starts parsed specs,
`ui/animation/animation_style.hpp` starts, ensures, or syncs animations from computed style and applies play-state changes,
`ui/animation/runtime.hpp` drives active entries with tick/completion/dirty reporting, callback-safe retained fills and post-callback sampling, reduced-motion timing policy, pause/resume, target/name state/pause/stop/restart, id restart,
`ui/animation/loop_bridge.hpp` ticks animations and requests frames only for active/finishing work, and
`ui/animation/easing.hpp` provides linear/bezier/steps easing, `ui/animation/spring.hpp` integrates
retargetable springs, `ui/animation/spring_runtime.hpp` samples active spring entries and snaps them under reduced motion, `ui/animation/style_overlay.hpp`
overlays samples onto `ComputedStyle` per target, `ui/animation/transition.hpp` parses transition
shorthands and longhand lists, `ui/animation/transition_runtime.hpp` starts matched transitions, diffs computed styles, and supersedes active target/property entries. Runtime assembly, tick ordering, fills, reduced motion, and the loop bridge are documented in [`docs/design/animation-runtime.md`](../design/animation-runtime.md), `ui/frame_pipeline.hpp` composes the animation tick, per-target dirty marking, damage tracking, and normalized redraw planning for one frame, and
`ui/text/text_measure_cache.hpp` bounds measurement caching (wired into the
Yoga measure callback). Font files load through FreeType (`ui/text/font_face.hpp`) with real
metrics (`ui/text/freetype_metrics.hpp`) and shape through HarfBuzz (`ui/text/text_shaper.hpp`,
`ui/text/harfbuzz_shaper.hpp`). Glyphs rasterize through FreeType (`ui/text/glyph_raster.hpp`), a
bounded deterministic atlas packs them (`ui/text/glyph_atlas.hpp`), and `TextRenderer`
(`ui/text/text_renderer.hpp`, `tessera_text_render`) batches atlas quads through
`Batched2DRenderer`; `rhi::Texture::write` updates atlas pages in place. The HAL has a steady clock and bounded idle wait
(`src/hal/timer/`, `src/runtime/system_clock.*`); the scene has focus/keyboard dispatch
(`ui/scene/focus.hpp`) with `tab_index` ordering, modal trapping, and `:focus` style re-evaluation;
and the batched 2D renderer has a damage-scissored `begin_damaged` path
with null-backend draw records. Async primitives (`ui/runtime/async.hpp`) add cancellation tokens
and once-only callbacks, a bounded deterministic `ManualExecutor`, and a thread-safe prioritized
`CompletionQueue` drained by `RuntimeLoop` under a per-tick budget; `WorkerExecutor`
(`runtime/worker_executor.hpp`) owns bounded worker threads, drains queued work on shutdown, joins
every thread, and can post prioritized completions. Lazy `Task<T>`/`Task<void>`
(`runtime/task.hpp`) coroutines post every resume to the `CompletionQueue`, propagate
results/exceptions at the `co_await` boundary, and observe cancellation before each resume.
`IoExecutor` (`runtime/io_executor.hpp`) caps in-flight blocking jobs on the worker pool and posts
completions back to the queue.
`runtime/diagnostics.hpp` tracks
per-frame/total counters. The structured diagnostics core is `src/diagnostics/`:
`diagnostics::Logger` (levels, categories, path redaction, bounded ring sink, disabled fast path),
`diagnostics::MetricsRegistry` (fixed-capacity atomic counters/gauges with snapshots), bounded
RAII tracing spans with Chrome Trace JSON (`diagnostics/trace.hpp`), and an overlay line model
(`diagnostics/overlay.hpp`). The lavapipe golden harness compares
`tests/golden/baseline/batched2d/dashboard.png` bit-exactly through
`rendering/golden/image_diff.hpp` and `batched2d_poc --compare` (`batched2d_golden`, Linux with
lavapipe). The open-questions register's runtime slices now have their first implementations:
`tessera_automation` (`include/tessera/ui/automation/`, `src/ui/automation/`) provides in-tree
SHA-256, salted hash64 UiIds keyed by the tessera.json namespace with explicit-`automationId`,
duplicate and collision diagnostics, and the preorder snapshot builder plus canonical JSON
serializer; `tessera_virtual` (`include/tessera/ui/virtual/`, `src/ui/virtual/`) provides a
fixed-capacity Fenwick prefix sum of 32 px main-axis estimates, refine-on-measure suffix shifts,
the section 6 re-anchor math, and a bounded keyed `ItemStateStore`, plus the read-only
`VirtualDataSource` with its fixed-capacity `KeyIndex`, the bounded `NodeRecycler` keyed to
`ui.virtualListPoolCap` (clean-on-acquire, stale-lease rejection) and `VirtualRange`
visible-range/re-anchor integration with a 10k-item allocation-free churn test, keyed focus/selection
in `ItemStateStore` (focused and selected entries pinned against LRU eviction, refusals counted),
`VirtualRange` `focus_item`/`step_focus` with exactly one realized `PseudoState::Focus` node and
`selected_items()` in data order, `scroll_to_index` Start/Center/Nearest (pure query, clamped to
`[0, total - viewport]`), and `VirtualGridRows` row-granular grid geometry, plus
`CollectionSemantics` (`include/tessera/ui/virtual/collection_semantics.hpp`): set size and 1-based
`pos_in_set` from the key index, realized `(UiId, ItemKey)` identity read off the realized node, the
active descendant, selection in data order, and bounded coalesced focus/selection announcements (one
per observed change, superseded dropped, allocation-free); tests `virtual_collection_semantics`,
`virtual_announce`. Next for this lane: the a11y tree/bridge consumes it; `tessera_boundary`
(`include/tessera/ui/boundary/`) provides `BoundaryTable` routing, the single sync catch point
`run_guarded`, and the decided retry policy (250 ms doubling to a 4 s cap, five retries,
`TESSERA_BOUNDARY_LOOP`). `tessera_scale` (`include/tessera/ui/scale/`) holds the render-scale
ladder (`quantize_render_scale`: 1.0/1.25/1.5/2.0, nearest with ties up, clamped) and
`WindowRenderScale` (one scale per window, recomputed only on a DPI/monitor change), while
`ui/text/scale_glyph_atlas.hpp` keeps one glyph atlas per ladder step with per-step invalidation;
tests `automation_*`, `virtual_*`, `boundary_*`, `scale_*`. Next: wire `ScaleGlyphAtlas` into
`TextRenderer` (draw selects `atlas_for(render_scale_)`, page textures keyed by ladder index, and
a ladder change invalidates the previous step). Paint-only tick interpolation is live:
`runtime::interpolation_alpha` (`fixed_step.*`), the `PaintSample`/`PaintSamples` model in
`ui/frame_interpolation.hpp` (`src/ui/frame/interpolation.cpp`), the `paint::PaintView` seam and
`FramePipeline::tick(root, dt, alpha)` with `FramePipelineConfig::interpolation` as the explicit
switch (`animationInterpolation` is not read by any config loader yet); interpolated nodes add their
painted box to damage, layout/text never interpolate, and border colors are paint-only dirty. Tests
`fixed_step`, `frame_pipeline_style_sync`, `frame_pipeline_overflow`, `animation_pipeline`. Next:
pass `RuntimeTick::alpha` from the runtime driver and decide the alpha wake threshold. `tessera_harness`
(`include/tessera/harness/`, `src/harness/`) holds the V1 headless-automation protocol codec:
strict JSON-lines framing (1 MiB cap, CRLF/EOF, per-line UTF-8), `LineTransport` with an in-process
pair, the versioned request/response envelope with stable error codes, the snapshot/action/trace/
audit command and result models, and canonical JSON; tests `harness_*`. The stdio adapter, bounded
session executor, host handler seam and CLI are in (`stdio_transport.hpp`, `session.hpp`,
`handler.hpp`; tests `harness_stdio_transport`, `harness_handler`, `harness_session`,
`harness_session_stream`, `harness_cli`). The deterministic script runner and a test-only fake HAL
landed, and the runner is wired to the real engine: `automation::EngineAutomationHost`
(`include/tessera/automation/engine_host.hpp`) borrows scene root + `AppHost` + `RuntimeLoop`, builds
the snapshot from the automation builder over the live tree (bounded: 65536 nodes, depth 64,
256-byte paths), and routes click/type/focus/scroll through the owned `InputPump`/`FocusManager`
with at most one settle frame per action; `AutomationDiagnostics` records one instant per command
when enabled and is proved byte-identical to no seams when disabled. `tessera test --script FILE
[--out DIR] [--report PATH] [--mode structural|audit]` and `tessera headless --mode ... --script`
are promoted, run one `RuntimeLoop` tick per `ScriptRunner::pump` with a `ManualClock` and a tick
`SessionClock`, write `report.json` plus trace/audit artifacts, and exit 0 pass / 1 run failure /
2 invalid script; `--out`/`--report` accept Windows separators. Landed since: blocking stdio wait
(`LineTransport::wait_readable`, `tessera headless` without `--script` serves until EOF),
`itemKey` list identity, latency audits (`--mode audit --profile`, `latency_audit.hpp`), the
`app.config` manifest loader and `ManifestScene`, the offscreen golden core
(`tessera_harness_image`/`tessera_harness_capture`, `check_frame`), and the CLI golden lane:
`tessera test --script F --mode golden|gpu-smoke --baseline PNG [--update-goldens] [--font TTF]`
runs the script structurally, renders the final scene (`cli_frame.cpp`), writes
`frame.png`/diff/`frame.compare.json` and a `frame` report object, and exits 0 match / 1 mismatch or
missing baseline / 2 mode unavailable (golden needs lavapipe via `VK_ICD_FILENAMES`).
`--update-goldens` is golden-only (ADR-009). Next for this lane: commit real baselines under
`tests/golden/baseline/harness/` and run them on lavapipe CI (Linux only). The `desktop` and
`mobile` latency profiles pass on a real clock on the Windows dev machine (six-step fixture script:
p95 3.4 ms desktop, 1.9 ms mobile, 2026-11-12); noisy shared runners should keep using `ci`. `v.set(x, { duration, easing })` and `v.set(x, { spring })` are lowered into `animSet` IR timing (validated, `TESSERA_IR_ANIMATION`); codegen emits an `AnimatedScalar` per animated value, ticked through `UpdateScheduler::tick` before `flush` (`UpdateTicket::tick`); reduced motion reaches generated components (Windows OS query at startup; no live change events or other platforms yet); animated values can run on a spring (`v.set(x, { spring })`); `v.set(x, { onComplete: handler })` lowers to `animSet.onComplete` (a `handler::N` id, validated; runtime delivery lands with codegen); completion callbacks and a host owner for `SpringRuntime` (style-property springs) are not wired. `import "./x.css"` is resolved by lowering into `IrModule::stylesheets` (missing file: `TESSERA_IR_STYLESHEET_MISSING`; codegen warns `TESSERA_CG_STYLESHEET_NOT_EMBEDDED` until embedding, issue #81). `tessera.json` `ui.styles` CSS files reach the harness `--app` scene after the built-in sheet. Script `assert` (exists/visible/enabled/focused/selected/text/name) and `waitFor` (one-shot `state` check) read the snapshot and fail with `assertionFailed`. The `scrollIntoView` action reveals realized items
(`ui::scroll_into_view`); revealing an unrealized virtual item through `VirtualRange` is still open. Manifest scenes already report the manifest structural
path and component id in snapshots (`NodeAnnotations`); a static `aria-label` (else `label`, else literal text) is carried as the manifest `name` and reported as the snapshot `name`. The POSIX `poll` path is not yet compiled anywhere. `tessera_accessibility`
now carries the in-tree AT-SPI2 client subset: D-Bus address parsing and a `Transport` seam with a
loopback fake, the bounded wire codec with golden bytes, SASL EXTERNAL, role/state/interface
mapping, and Accessible/Application/Action/Component/Text/Value dispatch with a bounded action
queue plus a bounded coalescing event queue (spec §12.1); tests `a11y_*`. Next: the `AtspiSession`
connect/auth/Hello/registry handshake and its pumped worker thread: `AtspiSession` owns the two
D-Bus connections (session connect/auth/`Hello` -> `org.a11y.Bus.GetAddress` -> accessibility
connect/auth/`Hello` -> `Registry.RegisterEvent` for state/property/bounds changes), with typed
`SessionError` values, a bounded `pump` on one joined worker thread, and read-only `Properties`
Get/GetAll; tests `a11y_atspi_session`, `a11y_atspi_session_pump`, `a11y_atspi_properties`. Next:
wire the session into the Linux host (dirty-driven snapshot publishing, actions drained at a tick
boundary, `Disabled`/`Timeout` -> accessibility off with a structured diagnostic), then
`PropertiesChanged` and reconnect/backoff. Windows UIA is `src/ui/accessibility/uia_*`: one MTA
bridge thread (`COINIT_MULTITHREADED`, bounded pump, `RPC_E_CHANGED_MODE` ->
`ApartmentUnavailable`), an allocation-free single-slot `SnapshotHandoff` (drop-oldest with a
counter), `UiaBridge` phases/job ring/16 ms idle wait/5 s liveness/ordered join+restart, and the
provider surface (IRawElementProviderSimple/Fragment/FragmentRoot + Invoke/Value/SelectionItem/
Selection) marshaled per call, with the COM adapter behind `if(WIN32)` (spec §12.2; tests
`a11y_uia_*`). Host wiring landed: `hal::Window::set_accessibility_root` is the seam, the Win32
wndproc answers `WM_GETOBJECT` with `UiaReturnRawElementProvider` only for `UiaRootObjectId` and
falls back to `DefWindowProc` for every not-ready case (one atomic load, never a COM/UIA wait on the
window thread), `mark_dirty()` + `tick(source, handler)` publishes only when dirty (a static tick
does zero accessibility work), drains actions at the tick boundary (≤8/tick, arrival order) and
posts one event-flush job per tick, and events are a pre-order NodeId diff (one root
ChildrenInvalidated, per-parent child-list changes, name/value/help-text/enabled/selected/bounds
PropertyChanged, one focus event) coalesced in a bounded 64-entry queue with drop-oldest counting;
the bridge adopts the pending snapshot immediately before each job. Tests `a11y_uia_events`,
`a11y_uia_host_getobject`, `a11y_uia_host_tick`, `a11y_uia_host_events`, `a11y_uia_window_getobject`
(Windows only). The host wiring landed: `get_HostRawElementProvider` answers with a
`UiaHostProviderFromHwnd` provider built on the bridge thread from the window's opaque handle, and
`WindowHost` owns one accessibility controller (`src/app/accessibility_host.{hpp,cpp}`,
`window_accessibility.cpp`, `accessibility_host_atspi.cpp`): on Windows it forwards
`hal::Window::accessibility_host_handle()` to the platform seam, installs
`make_window_root_provider(UiaHost&)` and uninstalls it before the window dies; on Linux it starts
`AtspiBridge` + `AtspiSession` and lets that session's own worker thread pump the bus while the UI
thread only publishes dirty snapshots and drains bounded actions. A failed/unsupported start and a
terminal AT-SPI phase both turn accessibility off with exactly one
`TESSERA_A11Y_UNSUPPORTED: <backend>: <error>` line and leave the window fully functional; a static
window rebuilds no snapshot. Tests `app_accessibility_host` (fake UIA apartment/platform seams and
the loopback AT-SPI transport with a synthetic peer); the Narrator/NVDA and Orca checklists are in
spec §12.3. Still open for this lane: event payload variants and installing the AT-SPI side on a real
desktop session. `tessera_schema` now loads and
validates shader manifests (`include/tessera/schema/manifest.hpp`): the strict §1–§3 shape with
closed key sets, the §4 rules including the V1 compute rejection (`ComputeStageUnsupported`),
`std::expected<Manifest, ValidationError>` with stable codes and JSON paths, and the §9 caps; tests
`manifest_*`. Next: call it from the shader build step before `glslc`. The front end enforces its
five `jsx::Limits` (file size 1 MiB, 200k tokens, depth 128, 100 diagnostics, 64 components) with
stable `TESSERA_JSX_LIMIT_*` codes, `excerpt()` 32-byte / sink 256-byte text caps so hostile input
cannot bloat a report, and a deterministic stress corpus (`compiler_limits`); the four downstream
budgets are documented in docs/design/security.md §3.1/§3.2 for the codegen lane. ICU is an opt-in
static dependency: `third_party/icu` pins icu4c 76.1 and builds static `tessera_icu_common` +
`tessera_icu_i18n`, `reduced_data.cmake` trims the data to 22 locale bundles (17,293,904 B) and
emits `locales.json` + `icu_locale_manifest.h`, `ui/text/icu_runtime.*` installs the blob with
`udata_setCommonData` (an in-process test proves no shared ICU is loaded), `ui/text/text_breaks.*`
exposes one UAX #29/#14 break API with the builtin rules kept as the ICU-free backend, and the
greedy wrap now consumes it: `layout_text`/`layout_text_measured` delegate to
`layout_text_with_breaks` (`src/ui/text/text_wrap.*`), which places one UAX #14 segment per line unit,
trims both segment edges (leading spaces never start a line), and keeps the blank-paragraph rule, the
over-wide-run split (binary search for the measured prefix, then the last legal break) and the
nowrap path; `text_measure.cpp` lost its duplicated wrap. Fixture deltas are deliberate and pinned
(interior space runs advance their real width; a line fills to a hyphen; em-dash BA class differs
between the builtin and ICU engines and both are recorded). The off-by-default switch is still
`TESSERA_ICU_REDUCED_DATA`; the condition the earlier note set is met, so the CI flip is queued as a
dedicated `icu-text` job (provision pinned icupkg/genccode + the full data blob, ON configure,
filtered ctest) while every default lane stays OFF - linking the 17.3 MB blob into every
`tessera_text` consumer grows that test binary from 0.84 MB to 19.7 MB, so it should not ride every
job. `ui/text/locale_resolver.*` validates locales against the manifest with `icu.locale.fallback` /
`icu.locale.missing` diagnostics; tests `icu_locale_pack`, `text_breaks_parity`, `locale_resolver`.
CSS `white-space` modes ride the same segment wrap: `TextStyle::white_space`
(`WhiteSpace{ Normal, NoWrap, PreWrap, BreakSpaces }` in `ui/text/text_measure.hpp`) collapses
interior runs to one advance under `Normal` (the default; pivot fixtures re-baselined and pinned in
text.md §6.1), keeps the pinned literal single-line path for `NoWrap`, preserves runs for `PreWrap`
and makes every preserved space its own line unit under `BreakSpaces`; the mode is carried into the
paint predicate and both layout/measure cache keys, and per-mode line boxes are byte-identical
across the builtin and ICU backends. The paint-time collapse stage landed: `ui/text/text_paint.hpp` walks a laid-out line as
`PaintedSegment`s, so the painted sequence now matches the measured line widths for every mode -
`Normal`/`NoWrap` collapse interior runs (and `NoWrap` renders one edge-trimmed line), `Pre`/`PreWrap`
preserve every byte, `BreakSpaces` keeps each preserved space as its own line unit, and a bidi line is
transformed per visual run. `text-overflow: ellipsis` landed on top of it: the property is a single
value (`clip`/`ellipsis`, a two-value shorthand is rejected), and at paint time a line wider than its
content box is cut on a cluster boundary to the longest prefix plus one U+2026 that still fits
(nothing when the ellipsis alone does not fit), per visual run for bidi text; layout, line boxes and
the measure cache are unaffected. `text-transform` and `word-spacing` stay open.
The app host lane (`src/app/`, `include/tessera/app/host.hpp`) connects the finished pieces:
`AppHost` runs the docs/design/app-host.md section 3 order for one scene root (animation tick ->
dirty collection -> selector tree rebuild -> Yoga layout + `apply_layout` -> `sync_style` ->
damage/redraw plan -> paint), owns the compiled stylesheet, `SceneStyleTree`/provider, layout
engine, `DirtyPass`, `DamageTracker`/`DamageAccumulator` and `FramePipeline`, and borrows the root
and the `RuntimeLoop`; it never sees a window, swapchain or device. `tests/unit/app_host_tests.cpp`
covers first frame, idle (zero work), paint-only damage, resize/DPI and settling. Supporting slice:
`SceneStyleTree` now populates tag/id/classes/has_text from scene element metadata, so a compiled
stylesheet styles generated nodes by type, class and id (`tests/unit/scene_style_metadata_tests.cpp`).
The driver sample `samples/click-demo` landed: generated Demo + compiled stylesheet. Its runner is
now the `tessera_app_window` library (`include/tessera/app/window.hpp`): `WindowHost` owns the
platform window, Vulkan device/swapchain, Batched2D renderer and font/text stack and drives the
host loop (events -> `InputPump` -> scheduler flush -> style/layout/damage -> paint -> present,
resize/DPI/close, two frames in flight), and `capture_frame` renders one windowless offscreen frame
through the same path; the sample is a thin CLI over both (windowless `--capture` + `--click`,
`--windowed [--hidden --frames N]`). Tests `app_window` (capture parity + hidden-window smoke), `app_input` (pointer move -> `:hover`,
press -> `:active`, release -> generated Click, focus + typed text -> generated onChange through the
`InputPump`) and `click_demo_capture`; images in `docs/images/click-demo-{before,after}.png`. Next
for this lane: caret blinking (paint keeps its default phase until a host-owned `CaretBlink` is
wired; Focus/Blur would need a free binding slot or a focus notification seam). The platform cursor
shape landed with issue #191: the CSS `cursor` keyword is resolved to `style::CursorKind` by
`cursor_value.cpp` (one grammar for the validator, `@supports` and the sync), stored on the scene node
(`Node::cursor`, inherited), mapped onto `hal::CursorShape` by `app/window_cursor.cpp` and requested
from the platform only on a change; Win32 installs stock `IDC_*` cursors from `WM_SETCURSOR`
(`src/platform/windows/window_cursor_win.*`), X11 caches `XCreateFontCursor` glyphs and installs them
with `XDefineCursor` (`src/platform/linux/window_cursor_x11.*`), and `WindowMetrics::cursor` reports
the resolved shape (window-chrome-v1.md section 8.5; tests `window_cursor`, `window_cursor_fake`,
`window_win32`, `window_x11`). Generated controlled `TextInput`s publish
caret/selection/preedit decoration from `TextInputRuntime::publish_decoration` (after each handled
event and at the end of `apply_state`), shown by `click_demo --click --type Ada`
(`docs/images/click-demo-typed.png`). The window input path is covered: `WindowHost::post_input` feeds normalized
events into the same InputPump/InputRouter as OS input, and `tests/gpu/app_window_input_tests.cpp`
drives click -> generated state and pointer focus + typed text -> generated onChange on a real hidden
window + swapchain; `WindowHost::apply_metrics` feeds a metrics snapshot through the resize/DPI path
(same test asserts the reflow and damage target). The design captures landed: `tools/poc/design_mock` renders the startup P0/P2/P4 frames and
the system vs framework window chrome through the same capture path, writes
`docs/images/{startup-p0,startup-p2,startup-p4,chrome-system,chrome-custom}.png`, and
`docs/design/shell-design-preview.md` documents them as design mocks (not the phase scheduler or the
Windows custom title bar). The `AppHost` root now keeps a caller-chosen `display`/`flex-direction`
(host owns only width/height), so a shell with a sidebar can make the root a row.

The Windows lane is unblocked: MSVC now compiles the tree with `/utf-8` (sources are UTF-8
without a BOM), `FrameFailureGuard` (`include/tessera/app/frame_guard.hpp`) bounds consecutive
frame failures so a bounded `--frames` run always terminates, a busy slot is skipped until
`is_complete()` instead of waiting on the GPU from the UI thread, and `OutOfDate` recreates the
swapchain at the window's real size rather than at its own current extent (a documented no-op).
The idle and teardown debt is closed: `include/tessera/app/idle_policy.hpp` parks the window driver
on the platform wait until the pacer deadline (clamped, never past it, never spinning), no host
frame begins while `RuntimeLoop` says nothing is due, a not-due tick calls `end_frame()` and paints
nothing, a frame is presented only when the redraw plan has damage (forced frames still paint), and
teardown polls the submitted command lists under a bounded budget instead of `device->wait_idle()`;
`run()` is re-entrant (it re-attaches the input queue and a second run is covered by
`app_window_smoke`), and `WindowOptions::max_fps` caps the cadence. Package imports are live in
lowering and in the driver: `lower_project(files, module, packages)` resolves
`from "tessera-ui/Button"` to `<root>/Button` and a bare `"tessera-ui"` to `<root>/index`, then
follows the ordinary cross-file rules; `tessera_codegen` reads the `packages` array from
`tessera.json`, validates it and follows each file's imports, so a package's sources are not listed
on the command line (#70). Declared prop records now lower from the props type alias:
`ir::Prop` gained `optional` and a `?` prop is absent rather than a nullable value; the mapping
covers string/bool/int/float/color/length/node_ref, `T[]` -> `list<T>`, `T|null` -> `optional<T>`,
`children`, `() => void` callbacks and payload-record callbacks, and an unsupported spelling is
`TESSERA_IR_UNSUPPORTED` naming the prop. The parity test now compares full prop records. The
library components are no longer hand-built IR: a CMake step lowers
`tessera/components/{Field,Form}.tsx` with a small bootstrap registry and compiles the generated
table into `tessera_library_components`, while the host build compares the fresh unit against the
committed copy the cross builds consume, so stale generated data fails the build. `library_tsx_parity`
re-lowers the committed TSX in-process and compares full canonical JSON (spans, defaults, template
shape); only the stamped identity is normalized. Prop defaults are part of the TSX now (flat
literal-list defaults are lowered into `Prop::default_value`) and the Field/Form goldens are
byte-identical. Remaining for #70: cross-file props aliases and object-literal defaults (the IR has
no object type). The index-based identity band is gone: library components are stamped from the
fixed `tessera.library` salt, so inserting a component alphabetically keeps the other ids and
goldens (`ir_ids_stability`).

Next slices, in order:

1. Golden/software path: the lavapipe corpus now covers dashboard, overlay, text, and shapes
   bit-exactly (text with tolerance), and a CPU canvas (`--backend cpu`) covers solid-rect
   references on every platform. The crash-report metadata exporter landed (diagnostics/crash.hpp,
   spec section 6.2: bounded, redacted, canonical JSON); platform capture, symbolication and the
   uploader stay open. Wider CPU coverage is next.
2. B3 window/present path: the Windows HAL window, Vulkan Win32 surface/swapchain, Linux/X11
   window, Vulkan Xlib surface/swapchain, and bounded event wait with `runtime::wait_for_next_frame`
   landed; swapchain image render targets, Win32/X11 render+present, and the windowed POC
   pacing demo are wired; the windowed POC and both swapchain smoke tests submit asynchronously
   with two frames in flight, and the POC is driven by `RuntimeLoop` + `SteadyClock`. Device-loss
   recovery is wired: `app/recovery_policy.hpp` charges at most three attempts per run, rebuilds the
   device/swapchain/sink/renderer/text stack at the current metrics (or waits for the runtime's
   recovered tick), never waits device-wide on the UI thread, and a failed recovery ends through
   `FrameFailureGuard` with `stats.stalled`; `app_window_recovery` drives it. Wayland and driving the
   render-on-demand loop (`--on-demand`, scripted changes) with real platform input are next
   (`docs/design/window-resize.md` §0 lists the X11 limits).
3. Platform input translation beyond Windows pointer/wheel/key/text (Linux/X11 next; touch, IME,
   hover/active). Focus is complete on the scene side: tab order, `:focus`, `:focus-visible`
   (keyboard-only ring, cleared by pointer focus), and modal trapping are tested.
4. [x] Damage-redraw equivalence is pinned on the CPU canvas (damage_equivalence_cpu,
   docs/design/dirty-rendering.md 6.7): paint-only, layout move, resize, the area fallback and the
   scissor-count fallback all reproduce the full-redraw pixels byte-for-byte; the swapchain present
   path stays B3.
5. Incremental relayout: `LayoutScheduler` skips clean frames, patches independent subtrees, and now
   guards a cached result with a layout input signature (`ui/layout/layout_signature.hpp`: viewport
   bits, tree shape, every layout-affecting longhand, text; paint-only properties, colors, shadows
   and transforms are deliberately excluded). It is wired into `AppHost` (`layout_runs()` /
   `layout_cache_hits()` report the policy) and `layout_signature_tests` prove the guarded boxes are
   field-identical to a fresh reflow. The one remaining gap is general Yoga partial reflow (Yoga has
   no partial-layout API, so a miss still free-layouts the whole tree). The earlier hit-rate
   "gap" was a real bug elsewhere: `apply_root_size` handed `Node::set_inline_style` the address of a
   temporary, so the root's computed `width`/`height` were freed heap bytes that changed between
   frames - the guard correctly missed. The host now formats the pixel values into stable
   `std::array<char, 32>` members and a paint-only inline edit hits the guard (0 runs, +1 hit), with
   width/text/viewport changes each running exactly one pass (`app_host`, `layout_signature_tests`).
6. Big Step C: JSX lexer, expression/type/module parser with per-item recovery landed (`src/compiler/analyzer/`); typed native handlers, `bind::` pairing, list-loop and attribute rules landed; AST -> IR lowering, C++ codegen with the `tessera_codegen` CLI, state/effects/refs, inline styles, conditionals, keyed lists and controlled text inputs landed; component composition and the static `Children` slot landed (`NodeKind::Slot` + validator rules, `{children}` lowering, `emit_composition.cpp` host node + `build(parent, slot)` overload with caller-owned slot content and child-first lifecycle, goldens `Card`/`CardPage`); next is virtual collection events (`onScroll`/`onEndReached`), generated scroll wiring and measurement feedback through `ItemGeometry::refine_measure`, then component prop lowering and callback props (`library_components.cpp` injects the library `Field` from a `"tessera"` import as an ordinary component, `apply_props()` assigns value props and callback props bind as payload-record `std::function` on node events, with typed prop diagnostics and no validation rules in generated code; goldens `Field`/`FormPage`); next is the `Form` library component and its generated form-state binding. The `Form` component is
now a library component too: `library_components.cpp` registers it (`initial` name=value seeds,
`validate`/`onSubmit` callback props, `children` slot), codegen emits a lazy
`tessera::ui::forms::FormConfig` + `Form` controller on the caller, maps FieldIds to Field members,
encodes required/minLength/maxLength/pattern as `SyncRule` data (the library evaluates; no rule code
is generated), resolves Field value/error by `name`, binds `TextChange` into the controller, routes
submit/validate with first-invalid focus through a host `FocusManager`, surfaces errors only after
touch/validate/submit, and hash-guards `apply_props()` for scalar and list props; Field props that
the controller owns are rejected loudly inside a Form. Goldens `Field`/`Form`/`FieldPage`/`FormPage`;
tests `forms_lowering`/`forms_codegen`/`forms_form_codegen`/`forms_codegen_runtime`. Virtual
collection events landed too: `onScroll` (typed `ScrollEvent`) and `onEndReached` (no payload) are
accepted on VirtualList/VirtualGrid with loud rejects otherwise, and codegen emits
`scroll_virtual_<n>(float& scroll_offset, float viewport_extent)` which owns an `ItemGeometry`
seeded from the emitted constants (4096-item cap), refines it from realized item bounds via
`refine_measure`, re-anchors the caller's offset in place with `reanchor_scroll`, applies the
overscanned window through `apply_virtual_<n>`, then dispatches `onScroll` and `onEndReached`;
`onEndReached` is edge-triggered with a distance-based 200 px re-entry throttle (deterministic, no
wall clock) and generated state. Virtual geometry is dynamic too: `itemSize`/`estimateSize`/`gap`/
`rowHeight` accept a number literal or a numeric state/prop ref, generated
`update_virtual_geometry_<n>` re-evaluates them behind a FNV-1a hash guard and seeds once per change
(`ItemGeometry::reseed`), an estimate/gap change keeps every measured entry while a changed fixed
extent that seeds the estimate drops them, and the caller's offset is re-anchored with the old/new
prefix sums and clamped so the first visible key survives (`codegen_virtual_geometry_runtime`).
`orientation`/`columns`/`overscan`/`columnWidth` accept a literal or a component prop
(a prop-driven option becomes a mutable member read once per scroll sync; unknown
orientation falls back to vertical, non-positive columns clamps to column-width mode, and
computed/state refs stay loud rejects). Collection events are dispatched by that generated host entry point
rather than a scene binding - there is still no `EventType::Scroll`, which is the deferred scene-side
step. Test `codegen_virtual_scroll`; docs in compiler.md §11 and virtualization-v1.md §10/§14. The scene-side
`EventType::Scroll` landed after that, so generated collections no longer need the synthetic
path. Remaining for #70: object-literal defaults, which needs an IR record type.

B3 adds the window/swapchain path with resize/DPI/OS lifecycle, normalized OS events, and
mobile/foldable metrics. Keep the B1 RHI, batched 2D renderer, and scene graph stable while doing
so.

## 2026-11-12/13 night: blobs, identity, aliases, virtualization and the config loader

Landed in one unattended run, each slice its own PR with the local Windows gate plus a remote GCC
build on lab-02 (CI on GitHub is confirmation only):

- **Stylesheet registration and theme asset** (#114, #119): `AppHost` registers compiled
  stylesheets in import-graph order, library CSS goes into `@layer tessera-ui`, and a 20-byte `TTHB`
  blob carries the theme asset so it loads without parsing. Both are ADR-048 artifacts and both keep
  the no-embedded-sheet diagnostic out of the way for apps that embed them.
- **MSVC portability** (#116, #122): two `/WX` shadowing errors that clang and GCC accept —
  C4456 for reused locals and C4458 for a parameter hiding an `Impl` member. Recorded in
  `.claude/team-memory.md`: clang-clean is not MSVC-clean, so the Windows run after a merge is a
  real gate, not a formality.
- **Timeline semaphores** (#117, #120): one timeline semaphore per device generation, a binary
  acquire semaphore per slot, slot reuse checked with `vkGetSemaphoreCounterValue` and a bounded
  100 ms drain, so frames in flight are tracked without a fence wait on the UI thread. The lavapipe
  run then rejected an assumption in the test (`max_in_flight == 0` on software Vulkan), fixed in
  #120; GCC's `-Wextra` also rejected a mixed enum/flags conditional there, which is the same lesson
  as the MSVC ones.
- **Stable component identity** (#118): emitted ids are a function of salt, source path, name and
  occurrence, so inserting a component keeps every other id and the goldens; library ids use a fixed
  salt instead of a band.
- **Cross-file props type aliases** (#121): `import type`, `as` renames, re-export chains and
  package roots resolve to the same props as an inline alias, with typed diagnostics for unresolved
  names, cycles and non-alias targets.
- **Dynamic virtual geometry** (#126): the four geometry options accept a component prop and a prop
  write re-realizes the window on the next sync while keyed items keep their identity.
- **Runtime config** (#127, #128): `parse_runtime_config` implements the normative spec (clamp vs
  error per the field table, strict unknown keys per section 4 rule 11, typed errors with offsets),
  and `WindowOptions::runtime` then consumes `pauseWhenOccluded` and `animationInterpolation`
  through a pure mapping layer, with a GPU test proving an occluded window stops presenting only
  when the key says so. Adaptive quality is the next consumer; the render-target scaling it needs
  does not exist yet.
- **Harness `scrollIntoView`** (#129): the action takes an item index and an alignment, drives
  `VirtualRange::scroll_to_index`, re-realizes and settles a frame, so a script can reveal and then
  address an item that was outside the realized window. The manifest scene gained a live collection
  per virtual node because it previously had only a scrollable axis.

Two slices had to correct their own brief: the config loader first shipped unknown keys as warnings
(the spec says errors) and the Android workflow gained an `adb` pre-step that ran before the SDK was
on `PATH` and made the lane red earlier than the flake it targeted. Both were reverted before merge;
the spec and the failing run each beat the plan.

## 2026-11-13 early hours: power, vector payloads and the Linux input stack

The second half of the same unattended run. Same protocol: one slice per PR from `origin/main`, the
Windows gate locally, the GCC build on lab-02 for anything platform-specific.

- **Adaptive quality, end to end** (#131, #136, #139): the controller reports a clamped
  render-scale decision with hysteresis; the host consumes it by rendering a downgraded frame into a
  smaller offscreen target and blitting it into the acquired swapchain image in the same submission,
  with one target per frame slot; and step 3 lowers the present cap to the next refresh divisor
  through the pacer's existing rate-change path. Scale 1.0 takes the unchanged direct path, so no
  golden moved and `set_content_scale` is never called. Device scale stays independent by
  construction — `glyph_raster_scale` takes only the reported device scale, so no call site can key
  the glyph atlas by the quality decision. The item stays open: step 1 (effect reduction) and step 4
  (pausing non-essential animations) do not exist, and a real display-refresh query is still missing,
  which is why `WindowOptions::refresh_hz` defaults to unknown.
- **Vector payloads** (#140): codegen synthesizes the SVG for a `Path` from its literal props,
  embeds it as an asset and emits a `PaintKind::Vector` payload pointing at it, so the existing
  asset → decode → path-rasterizer chain paints it with no new payload kind, no new build step and no
  new dependency. Asset names derive from the stable identity scheme, so inserting a node does not
  rename an existing asset; non-literal `d`/`viewBox`/`fill`/`stroke`/`strokeWidth`/`label` are typed
  errors instead of silent defaults.
- **Linux input stack** (#133, #134, #135): XInput2 valuator scroll, touch and pen translation plus
  an IME pre-edit state machine, with the pump selecting XI2 when present and falling back to the
  unchanged core path. Two follow-ups were needed before the configuration was honest: the helpers
  that exist only for the XI2 build had to move inside the `TESSERA_X11_XI2` guard, because the
  fallback configuration on the GitHub runner builds without `libxi` and rejected them as unused
  functions; then GCC 13 rejected the pre-edit splice as `stringop-overflow` for a `spliced[size++]`
  loop whose break guard it could not use, so the splice copies each segment as
  `min(room, available)` instead. g++ 15 on lab-02 accepted both original forms.

Three lessons, all now in `.claude/team-memory.md`:

1. **A gate machine is only a gate for what it can build.** lab-02 reported `X11 found` and
   `XInput2 found` after its packages were installed, so it could not exercise the fallback
   configuration the GitHub runner actually compiles. The opposite is also true: lab-02 has no
   ThorVG, so the vector paint golden ran on Windows only and the Linux pixel check had to wait for
   CI. State which configuration a gate proves, or it proves nothing.
2. **Compiler versions disagree, so both compilers have to run.** Three portability failures in one
   night — two MSVC shadowing errors and one GCC 13 range-analysis false positive — against code that
   clang accepted with `-Wall -Wextra -Wpedantic -Werror`.
3. **Write the bound the compiler cannot infer.** The fix for the GCC 13 warning was not a
   suppression but restating the loop as a bulk copy; the same shape is safer for the next reader.

## 2026-11-13, last hours: closing the power ladder and a CI lesson

Four slices finished the loop the second half started:

- **Present cap** (`src/runtime/present_cap.{hpp,cpp}`, #139): a pure function of (refresh,
  `maxFps`, quality step) moves the cap to the next refresh divisor below the current effective rate,
  and the pacer applies it through its existing rate-change path at the next frame boundary. The walk
  **stops** at the last divisor at or above 10 Hz instead of clamping to 10 Hz, because a clamp would
  name a rate the display cannot produce and would break "every value is a refresh divisor".
- **Display refresh** (`src/hal/window/refresh_rate.cpp`, #143): the divisor needed a source, so the
  HAL gained `display_refresh_hz()` — Windows reads the current mode of the window's monitor, X11
  uses the active RandR CRTC of the output under the window, and everything else stays unknown. 0
  means unknown, never a guess, and an explicit `WindowOptions::refresh_hz` still wins. The queried
  value is deliberately not pushed into the pacer's uncapped base pacing: doing that wall-clock-gated
  the deterministic `pump_once()` seam for every default window and broke five GPU tests.
- **GPU frame time** (`include/tessera/rendering/rhi/timestamp.hpp`, #152): a capability probe plus a
  per-slot timestamp ring gives the controller a GPU sample when the device reports one and the CPU
  sample otherwise. The read happens inside the existing `is_complete()` slot-reuse branch, so there
  is no fence wait, no `WAIT_BIT` and no `wait_idle` — the ADR-021 rule survives an honest GPU
  measurement. The sample lags by up to two frames and the fallback counters make that visible.
- **Composable bundles** (`include/tessera/ui/assets/asset_bundle_compose.hpp`, #146): the vector
  payload slice had recorded a real limit — `VectorPaintConfig` took one bundle, so an app embedding
  its manifest could not paint a generated vector. The composition is a borrowed span searched
  first-wins; merging into a new bundle would have copied every blob at startup to answer a lookup
  that a binary search already answers.

### The lesson that cost the most time

Slack in a gate is not the same as a green gate. Android had been **red on `main`** from `8e27260`
onwards and was reported green three times, because every push cancels the in-progress run of the
same workflow: the newest row said "in progress" or showed the *current* commit while the last
completed run on an earlier commit had failed. The failing binary was `ir_ids_stability_tests`, a
repo-file reader that needs the library `.tsx` files from the checkout, which the emulator sandbox
does not have — the same reason a dozen other binaries were already skipped. Fixed in #150 by adding
it to that list; the check that would have caught it earlier is reading the ledger of completed runs
per workflow, not the newest row.
