# Web target: running an app in the browser

Status: first slice (issue [#290](https://github.com/thnak/Tessera/issues/290)); the Wasm app build of
the gallery and the CI publish job are not done. Normative decisions are in
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
| `host/tessera_web_host.js` | WebGPU check, DOM listeners, frame bridge; ES module, no build step |
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

## 5. Tests

ctest `web_input_translation`, `web_frame_scheduler`, `web_host` (C++, no browser) and
`web_host_glue` (Node, fake DOM; skipped when Node is absent).

## 6. Not done (needs emsdk or later slices)

- `web_exports.cpp`, `library_tessera_web.js` and `tessera_web_app` are written but **uncompiled**:
  the authoring machine had no emsdk. The web CI lane is their first build.
- The gallery does not build for the web: it links the Vulkan window driver (`tessera_app_window`). A
  web app driver (WebGPU device, canvas surface, `WebHost` frame step) must replace it first.
- No browser has run the page: Button page pointer/keyboard, and the zero-callback check in a real tab.
- CI: building the gallery on every PR and publishing from `main`; download-size and first-frame budgets
  (item 9); the hidden-textarea IME bridge, ARIA mirror, clipboard, files and persistence.
