# ADR-046: Web (Wasm + WebGPU) as a secondary target for running apps in the browser

- Status: Accepted (2026-11-11)
- Date: 2026-11-11
- Amends: [ADR-024](ADR-024-native-first-boundaries.md) items 6 and 9 (the browser
  becomes an allowed OS/HAL boundary for this target) and [ADR-002](ADR-002-webgpu-dawn-for-wasm.md)
  item 2 (WebGPU serves this target as well as design time). All other items of both stay in
  force.
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI),
  [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (goldens),
  [ADR-012](ADR-012-static-linking.md) (static linking),
  [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step loop),
  [ADR-021](ADR-021-async-runtime.md) (async runtime),
  [ADR-037](ADR-037-self-contained-wasm-designer.md) (Wasm designer),
  [ADR-040](ADR-040-generated-base-and-developer-class.md) (developer-owned C++, proposed).
- Terms: **web target** = a Tessera app (generated C++ plus developer C++) compiled with Emscripten
  to WebAssembly and run in a browser tab. **Designer** = the design-time tool of ADR-037, which
  runs IR through an evaluator; it is not the web target. **COOP/COEP** = the HTTP headers that
  enable `SharedArrayBuffer`, which Wasm threads need.

## Context

Native installers are a poor way to share a demo of a new framework: people are asked to run an
unsigned binary from an unknown source, so many will not try it at all. A browser tab is
sandboxed, needs no install and works from a link.

The repo already plans most of the pieces for design time (Big Step D, ADR-002, ADR-037): the
engine compiled with Emscripten, a WebGPU RHI backend, and bundled fonts. That plan does not run
an app: the designer executes IR and only simulates native handlers through preview bodies, so
any logic written in C++ is missing. ADR-024 items 6 and 9 also limit Wasm to the design-time
viewer and state that production binaries contain no browser runtime.

Other frameworks with a browser target show where this goes wrong:

- **Download size and start time.** Qt for WebAssembly apps commonly reach tens of MB; Flutter
  web's CanvasKit renderer adds roughly 1.5 to 2 MB before any app code.
- **Text input in a canvas.** Canvas-rendered apps (Flutter web, Qt for WebAssembly) have a long
  history of IME, virtual-keyboard and composition bugs, because the canvas receives no text input.
- **Accessibility.** Screen readers see nothing in a canvas; Flutter web maintains a separate
  semantics DOM tree to fix this.
- **Threads.** Wasm threads need COOP/COEP headers, which many static hosts (for example GitHub
  Pages) cannot set.
- **Browser conflicts.** Reserved shortcuts, page zoom changing the device pixel ratio, and
  scroll/gesture defaults.

## Decision

1. **The web target is a secondary target for demos, samples, trials and bug reproduction.**
   Native stays primary. No feature is designed web-first, the web target may lag behind native,
   and V1 makes no promise that it is suitable for production web apps. Raising it to a
   production target needs a later ADR.
2. **Same code path as native.** The web build compiles the generated C++ and the developer C++
   with Emscripten and links the same runtime, UI, layout, style, text and renderer libraries. It
   does not contain the IR evaluator, front end, designer, devtools mutation or any JavaScript UI
   runtime. ADR-024 items 1 to 5 apply unchanged: no interpreter or virtual DOM, no JS UI runtime,
   no UI objects across the boundary, static linking only.
3. **The browser is an OS behind the HAL.** A new `src/platform/web` implements the HAL: canvas
   as the window, DOM pointer/keyboard/wheel/touch events normalized into `InputEvent`, clipboard,
   file open through `<input type="file">`, save as download, persistence through OPFS or
   IndexedDB, visibility and resize as lifecycle events. UI code never sees DOM or JS objects.
   This adds the browser to the allowed OS/HAL boundaries of ADR-024 item 6, for the web target
   only.
4. **GPU through WebGPU.** The web target uses the WebGPU RHI backend of ADR-002 and Big Step D,
   through Emscripten's WebGPU bindings. Calls into the browser GPU API are platform API calls, in
   the same class as Vulkan calls on native; the batched renderer keeps their number per frame
   small. Without WebGPU the app shows a clear "unsupported browser" page instead of a blank
   canvas. A software fallback (CPU rasterizer presented through a 2D canvas) is optional later
   work, not part of this decision.
5. **Browser-driven loop.** `RuntimeLoop` runs one step per `requestAnimationFrame` callback.
   Render-on-demand holds: when nothing is dirty and no animation or timer is pending, no frame
   callback is requested, so a static UI uses no CPU or GPU. The loop never blocks the main
   thread.
6. **Single-threaded by default.** The default web build has no Wasm threads, so it runs on any
   static host without COOP/COEP. The worker executor runs as a cooperative executor on the main
   thread with a bounded time slice per frame; blocking work is not allowed. Native-only
   accessibility bridge threads (UIA, AT-SPI) are not compiled. A threaded build is an opt-in for
   hosts that can send COOP/COEP.
7. **Text input through a hidden editable element.** While a text control has focus, a hidden
   `<textarea>` is positioned at the caret, so the browser places the IME candidate window and
   the mobile virtual keyboard opens. `compositionstart/update/end` and `beforeinput` map to the
   existing normalized `Composition*` and text events, with offsets converted to UTF-8 bytes at
   the HAL edge.
8. **Accessibility through an ARIA mirror.** The existing accessibility tree is mirrored into a
   hidden DOM tree with ARIA roles, names, states and bounds. The mirror is updated when the
   accessibility tree changes, not every frame.
9. **Budgets from day one.** The web build has download-size and time-to-first-frame budgets in
   `performance.md`, measured in CI on the samples (Brotli-compressed `.wasm` plus data,
   `-Oz` and `wasm-opt`, the reduced ICU data package). The first budgets are provisional and are
   set from the first measurement; the sample must not grow past them without a reviewed change.
10. **Packaging and hosting.** A `web` CMake preset (Emscripten toolchain) produces a static
    folder: `index.html`, the `.wasm`, a data package with fonts and ICU data, and a small JS
    loader. It runs from any static host with no server code. The default build makes no network
    requests besides loading its own files, and has no analytics.
11. **Parity with native.** Running the same sample with the same scripted input (automation IDs,
    ADR-027) must give the same scene and state snapshots on native and web. Pixel goldens keep
    the software path as the source of truth (ADR-009); WebGPU output is compared with the
    tolerance ADR-002 item 5 already defines.

## Consequences

### Positive

- Anyone can try a sample from a link inside the browser sandbox, which removes the trust problem
  of native demo installers.
- The web target reuses Big Step D (Emscripten build, WebGPU backend) and adds a second full
  consumer of the RHI, so backend bugs show up earlier.
- The parity test checks the generated C++ path on two platforms with very different event loops
  and threading, which catches hidden assumptions (blocking waits, thread affinity, wall-clock
  use) on native as well.
- Developer C++ runs unchanged, so demos show real behavior, unlike designer preview bodies.

### Negative / risks

- A new platform to maintain (HAL, IME, accessibility mirror), even at secondary level.
- Browser WebGPU support differs by browser and platform; some visitors will get the unsupported
  page.
- Single-threaded execution makes long tasks visible as jank; the cooperative executor needs
  careful time slicing.
- Download size can grow quietly; budget enforcement in CI is required, not optional.
- Developer C++ that uses OS APIs, blocking I/O or threads directly will not build or run on the
  web; it has to go through HAL services.

## Alternatives considered

- **Designer only (status quo).** Rejected for demos: native handlers are simulated by preview
  bodies, so demos would not show real behavior.
- **Web as a primary production target now.** Rejected for V1: it would make browser limits
  (threads, WebGPU support, DOM accessibility) shape native design decisions.
- **WebGL2 backend for wider browser support.** Rejected for now: a second browser backend below
  the WebGPU floor of ADR-002, with its own shader path and goldens.
- **Signed native demo builds only.** Still useful, but signing does not remove the install step
  or the need to trust the publisher.
- **Streaming a native app as video.** Needs servers, adds latency and cost, and does not run
  offline.

## Follow-up

- `web` CMake preset and Emscripten toolchain file; a CI job that builds one sample and publishes
  it as a static artifact.
- `src/platform/web` HAL: window/canvas, input, lifecycle, clipboard, files, persistence.
- `RuntimeLoop` frame-callback driver and the cooperative main-thread executor.
- Hidden textarea IME bridge, depending on the composition fixes in issue
  [#9](https://github.com/thnak/Tessera/issues/9).
- ARIA mirror of the accessibility tree.
- Web budgets in `performance.md`, linked to issue
  [#13](https://github.com/thnak/Tessera/issues/13).
- On acceptance: add notes to ADR-024 and ADR-002, a web row in the platform tables of `hal.md`
  and `AGENTS.md`, and a guide for building and hosting the web demo.
