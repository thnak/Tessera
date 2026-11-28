# Web target: running an app in the browser

Status: first slice (issue [#290](https://github.com/thnak/Tessera/issues/290)), the web app driver
(issue [#1013](https://github.com/thnak/Tessera/issues/1013)) and the gallery on it (issue
[#1023](https://github.com/thnak/Tessera/issues/1023)); GPU-browser pixels, device recovery and the CI
publish job are not done. Normative decisions are in
[ADR-046](../../decisions/ADR-046-web-target-for-demos.md); this note records how the code is cut.

## 1. Shape

```text
DOM event --> tessera_web_host.js --> tessera_web_* (extern "C") --> WebHost --> InputQueue
                                                                       |
                  requestAnimationFrame <-- FrameCallbackScheduler <---+---> app frame step (RuntimeLoop)
```

The cut follows the testability line: everything that decides something is plain C++ in
`src/platform/web` with no browser or Emscripten header, so it builds and is unit-tested on every lane.
Only `web_exports.cpp` (the `extern "C"` surface and two JS imports) needs the Emscripten toolchain.

| File | Role |
| --- | --- |
| `web_input_translation.*` | Flattened DOM pointer/wheel/key samples to `ui::InputEvent` (item 3) |
| `web_frame_scheduler.*` | When a `requestAnimationFrame` may be requested (item 5) |
| `web_host.*` | Queue, scheduler, resize and visibility lifecycle, overflow counter |
| `web_exports.*` | `tessera_web_pointer/wheel/key/focus/resize/visibility/wake/frame`; Emscripten only |
| `web_surface_extent.*` | The canvas backing-store size rule (css size times device pixel ratio, clamped) |
| `web_canvas_surface.*` | The web app driver's GPU half: the page's device as an `rhi::Device`, the canvas surface, one render target per frame; Emscripten only |
| `src/app/web_driver.*`, `web_gpu_stack.*` | The web app driver proper (`tessera_app_web`): `AppHost` frame, input pump and canvas frame over the two rows above; text, vector and glass stacks on the WebGPU device |
| `host/tessera_web_host.js` | WebGPU check, device request, DOM listeners, frame bridge; ES module, no build step |
| `host/index.html` | Canvas plus the "unsupported browser" page (item 4) |
| `host/library_tessera_web.js` | The two imports C++ calls, forwarded to the frame bridge |

## 2. Input rules

- Coordinates are canvas-relative CSS pixels, the logical unit `InputEvent` uses everywhere; the device
  pixel ratio reaches the engine only through `WebHost::metrics()`.
- Modifier bits, pointer types, phases and wheel modes are a fixed numeric contract between the page and
  `web_input_translation.hpp`; both sides are tested.
- Wheel deltas become notches with the web sign: 100 px, 3 lines, or one page (ten notches) per unit.
- `KeyboardEvent.key` maps to the logical `ui::key` codes (letters upper-cased). A key-down that types
  also queues a `Text` event, except with Control or Meta (shortcuts); Control with Alt is AltGr and
  types. Malformed UTF-8 becomes U+FFFD. The hidden-textarea IME path (ADR-046 item 7) is not built; it
  is only needed when the gallery shows text controls.
- A touch contact is the left button, a pen reports pressure, an unknown pointer type behaves as a mouse.
- Ctrl/Meta chords are not `preventDefault`ed, so browser shortcuts keep working.

## 3. Frame callbacks (acceptance: zero when idle)

`FrameCallbackScheduler` requests a callback only on a wake (input, resize, completion, timer,
visibility) or while the last tick reports pending work, a pending completion, or a due timer, and never
while one is outstanding. A parked page holds no callback; a future timer arms one `setTimeout` that
wakes it. A lost device retries on a timer, never a rAF spin. A hidden page requests nothing and
repaints once when shown. `web_host_tests` counts the requests; `host_glue_tests.mjs` checks the JS
bridge holds at most one pending rAF and one timer.

## 4. Building an app

Under the `wasm-webgpu` preset (Emscripten), `tessera_web_app(<target>)` in `src/platform/web` turns an
executable into the static folder: `app.js` + `app.wasm` (MODULARIZE, single-threaded, so no
COOP/COEP), `index.html` and `tessera_web_host.js`. The app's `main` creates a `WebHost` with
`js_callbacks()`, calls `attach_global_host`, registers its frame step with `set_frame_fn` and
returns; the browser drives the rest.

### The web app driver (issue #1013)

The Vulkan window driver's job on the web is split between the page and `WebCanvasSurface`. `boot`
requests the adapter and device *before* instantiating the module and passes it as
`Module.preinitializedWebGPUDevice` (a refused adapter or device shows the unsupported page), so the
C++ side only wraps a ready handle and never waits (ADR-021). `WebCanvasSurface::create("#tessera-canvas")`
wraps that device as an `rhi::Device`, binds the canvas through an emdawnwebgpu surface and reads the
preferred canvas format (BGRA8 or RGBA8, never a guess); `configure(backing_extent(metrics))` follows
resizes; `acquire()` returns the current canvas texture as a presentable render target. The browser
presents it when the task ends, so there is no present call. A WebGPU command list is single-use and
cannot be submitted synchronously: the app records a fresh list per frame, `submit_async()`s it and keeps
at most two in flight, polling `is_complete()`. When both are in flight the frame stays owed and a
due timer asks for the next callback.

`samples/web_render` is the minimal app on that driver: a batched 2D card with a hover and press button,
painted only when the pointer changes it. Without a device (Node) it runs the host contract and draws
nothing.

### The gallery on the web (issue #1023)

`samples/gallery` builds a second target, `gallery_web`, from the same generated pages, stylesheets and
theme as the native gallery. `web_main.cpp` is the whole difference: it reads the page from the URL query
(`?page=form&theme=instrument-dark&density=compact&text-scale=125&rtl=1`), builds a heap scene
(`gallery::make_scene`, one page table with `run_page`) and hands it to `app::WebAppDriver`
(`src/app/web_driver.*`, library `tessera_app_web`). The driver runs the same `AppHost` frame as
`WindowHost`: DOM events from `WebHost` go through the `InputPump` into the scene, the loop ticks, and a
changed scene is one canvas frame through the batched 2D renderer. The pieces it shares with the Vulkan
window driver are the `TextStack` (now `text_stack.hpp`; the web build skips its worker thread and runs the
glyph scale step synchronously), the `VectorStack` (the backend's compute descriptors are a constructor
argument, so the WGSL set plugs in) and `window_aa_policy`. The glass pipelines are the same two shader ids
(`batched2d`, `glass_blur`) resolved to their WGSL twins. Shared gallery code moved to `resources.*`
(the CSS, theme and stylesheet loading) so the two builds read their inputs one way.

The CSS, theme asset and Plex Sans Regular are `--preload-file`d into the module file system at `/gallery`
and shipped as `app.data`, so the budget has a `data_gzip` entry. The page generation runs the codegen CLI;
in the Emscripten build that is the Node build of the CLI (`NODERAWFS`), run through
`CMAKE_CROSSCOMPILING_EMULATOR`, so one `cmake --build --preset wasm-webgpu` produces the app with no native
tool. Without a WebGPU device (Node) the driver runs headless: fonts are measured, the stylesheet resolves,
layout runs and input is dispatched; only the canvas is missing (`tessera_render_gpu()` is 0).

## 5. Tests

ctest `web_input_translation`, `web_frame_scheduler`, `web_host` (C++, no browser) and
`web_host_glue` (Node, fake DOM; skipped when Node is absent).

Under Emscripten, `samples/web_smoke` is the minimal web app (no renderer; it counts frames and
consumed events). It is linked for `web,node`, and `scripts/ci/web_smoke.mjs` runs it under Node in the
`wasm-build` job of `.github/workflows/webgpu.yml`: a wake requests exactly one frame, further input
shares it, a frame runs the C++ step once and consumes the queued event, and a parked app holds no
pending callback. The same script enforces the gzip download budgets and the Node first-frame bound in
`tools/web_budgets.json`. `web_exports.cpp`, `library_tessera_web.js` and `tessera_web_app` compiled
and linked on emsdk 7.0.0 on the first try.

`web_render` goes through the same script (`--app web_render`; Node has no device, so it asserts zero
canvas frames) and has its own budget entry. `web_surface_extent` is a host ctest; `host_glue_tests.mjs`
covers the device request and the refusal paths.

`tools/webgpu_browser/run_web_app.mjs` drives the built `web_render` folder in a real Chromium through
Playwright (software WebGPU is enough, verified on thnak-server): the first canvas frame is submitted
at the backing-store size, a hover, press, release and leave are one canvas frame each, no-change moves
are none, no page error, and an idle page requests no `requestAnimationFrame`. Headless software
WebGPU screenshots show a blank canvas, so the pixel check (`--pixels`) is for a machine with a GPU.

`gallery_web` goes through both (`--app gallery_web` for each). Under Node the smoke asserts the
headless contract above and the budgets, including `data_gzip`. In Chromium with software WebGPU
(thnak-server) `run_web_app.mjs --app gallery_web [--query page=form]` checks the first canvas frame at the
backing-store size, that Tab (the canvas is focusable, `tabindex="0"`) repaints the focus ring, that a
pointer sweep repaints hovers and a settled page repaints nothing, no page error, and zero
`requestAnimationFrame` while idle; the icons page (the vector stack) draws its first frame without
error. Two defects only this run could find are fixed with it: the WebGPU backend rejected a pipeline
with no vertex buffer and did not register `glass_blur` (so no glass pipeline could be built), and a DOM
mouse carried Chrome's pointer id 1, which the router does not treat as primary, so nothing hovered
before the first click (a mouse is now pointer 0, like the native one).

## 6. Not done

- Pixels on a real GPU: `run_web_app.mjs --pixels` (for the gallery it checks the canvas is not one
  flat colour; for `web_render` three exact pixels) has not run, because headless software WebGPU
  screenshots are blank. The golden for a web frame is lavapipe only, so a web pixel comparison is a
  GPU-machine check, never a CI gate.
- Device loss and surface re-creation are not recovered: a lost device leaves the app idle.
- CI: `gallery_web` is built and smoke-tested on every PR in the `wasm-build` job; publishing it from
  `main` is not done.
- The hidden-textarea IME bridge, ARIA mirror, clipboard, files and persistence. Text fields in the gallery
  take the DOM key and text events the page already forwards, but not composition.
