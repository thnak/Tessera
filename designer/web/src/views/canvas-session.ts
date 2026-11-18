// SPDX-License-Identifier: MIT

// The Canvas view's session (P3, issue #167; the React host is issue #422).
//
// **This is where ADR-054 decision 3 is load-bearing.** The engine draws exactly one thing -- the preview
// canvas -- and the page remains its host. React renders the `<canvas>` element *once*, as a childless leaf
// with a ref, and never again: no changing props, no children, no key, so React never replaces or reconciles
// it. Everything after that is this session's: the backing-store size, the WebGPU context, the
// `requestAnimationFrame` that draws, and the failure states. A preview crash therefore loses the canvas,
// not the editor; and React's render cycle never runs while a GPU frame is being recorded.
//
// Four things this module is careful about, because each is a requirement rather than a detail:
//
//  1. **Render on demand.** A static scene draws once. A frame is requested only when something changed: an
//     applied edit that the module answered `changed:true`, a device-pixel-ratio change, a toolbar change, or
//     the pane becoming visible again. There is no `requestAnimationFrame` loop spinning at 60 Hz -- the
//     repository's idle rule (a static UI must present nothing) applies to the browser page too. The frame
//     callback is gated on the view being visible, so a hidden pane never draws.
//
//  2. **The unsupported path is a state, not a blank canvas.** `canvas-device.ts` turns every way of
//     failing -- no `navigator.gpu`, no adapter, `requestDevice()` rejecting, a module without the handover
//     export -- into a sentence the pane shows with a pointer at the Scene tab.
//
//  3. **The device handover is explicit and happens once.** A device is acquired on the first *apply* and
//     reused; re-running the handover per apply would leak a device per keystroke. A *lost* device is the one
//     case that re-acquires, and it says so.
//
//  4. **The module and the device are lazy.** The module is imported when the Canvas view is first opened, and
//     the device is acquired by the first apply there -- which, since #444, is the first automatic one rather
//     than a button press. The unsupported state is decided from `navigator.gpu`'s presence, which needs no
//     async work and no download, and is shown the moment the tab opens -- so a browser without WebGPU never
//     downloads `tessera_canvas.wasm`, and a visitor who never opens Canvas takes no GPU.
//
//  5. **Applying an edit is not a restart (issue #444).** `apply()` re-evaluates the running session and lets
//     the engine carry state by its own rule; a source that does not evaluate leaves the evaluation and the
//     picture alone. Only `run()` -- the **Restart app** button -- releases the session, which is what resets
//     every cell to its initializer. Nothing on the edit path may call `pane.hideStates()`, `showFailed` or
//     any other state transition: those are the ways a canvas goes blank, and a blank canvas mid-word is the
//     failure this slice exists to prevent.
//
// What the view does NOT claim: the pixels are the module's, but the comparison against the lavapipe golden
// is a CI/browser-lane job (`scripts/ci/scene_frame_parity.py`). This module cannot read pixels back
// synchronously, which is why no report it returns claims `pixelsAvailable`.

import { wasmModuleUrl } from "../lib/module-url";
import type { ProjectReader, StatusReporter, ViewSession } from "../lib/views";
import { callExport, requireExports, stageProject, type WasmModule } from "../lib/wasm-call";
import { acquireDevice, handOverDevice, type CanvasModule } from "./canvas-device";
import { loadFontFor } from "./canvas-font";
import {
  requestedViewport,
  sizeToDevicePixels,
  type CanvasApplyReport,
  type CanvasFrameReport,
  type CanvasStateCell,
  type CanvasViewportReport,
} from "./canvas-frame";
import { createCanvasPane, type CanvasPane, type CanvasPaneElements } from "./canvas-pane";

export type { CanvasApplyReport, CanvasFrameReport, CanvasStateCell };

/** The module factory `tessera_canvas.js` exports. */
type ModuleFactory = () => Promise<WasmModule & CanvasModule>;

export interface CanvasSessionOptions {
  elements: CanvasPaneElements;
  /**
   * What Run draws: the whole project and its entry, not the textarea's value.
   */
  source: ProjectReader;
  setStatus: StatusReporter;
  /** The toolbar's current `{width, height, dpr}`; a canvas run must not be stuck at 320x240. */
  environment: () => { width: number; height: number; dpr: number };
  /**
   * The toolbar's simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` |
   * `tabletop`. It is a layout input, so a change draws again rather than waiting for a Run.
   */
  foldPosture: () => string;
  /** Reports readiness to the page's shared Run button. */
  onReady: (ready: boolean) => void;
}

export interface CanvasSession extends ViewSession {
  readonly id: "canvas";
  readonly pane: CanvasPane;
}

export function createCanvasSession(options: CanvasSessionOptions): CanvasSession {
  const pane = createCanvasPane(options.elements, options.setStatus);
  const { canvas } = options.elements;
  const source = options.source;
  // The module factory, or null before it loads. `gpu` holds the device the page acquired.
  let factory: ModuleFactory | null = null;
  let gpu: {
    device: GPUDevice;
    queue: GPUQueue;
    module: WasmModule & CanvasModule;
    context: GPUCanvasContext;
  } | null = null;
  // Set when a frame has been requested and not yet drawn, so a burst of changes makes one frame.
  let framePending = false;
  // The dpr the canvas was last sized for; a change resizes it (a real, drawable event).
  let lastDpr = 0;
  // Cleared while the view is hidden, so a hidden pane never draws.
  let visible = false;

  // Sizes the backing store and remembers the dpr it used, so a resize can tell a real change from noise.
  function resize(): { dpr: number; width: number; height: number } {
    const sized = sizeToDevicePixels(canvas, requestedViewport(options.environment));
    lastDpr = sized.dpr;
    return sized;
  }
  // Draws the current source. Called from a `requestAnimationFrame` callback, never from the input handler,
  // so a fast typist cannot make the paint path reentrant and the canvas updates in step with the display.
  //
  // **Two module calls, not one, since #444.** The old path was `tessera_canvas_frame`, which evaluates
  // and paints in one stateless call -- so every keystroke re-evaluated from scratch, the state cells
  // were gone with the tree, and a failed parse made the module `clear_canvas` and hand back an error:
  // a blank rectangle exactly while a developer was mid-word. Now the session *applies* the source
  // (`tessera_canvas_apply`: evaluate, carry state, report whether a frame is owed) and then *draws*
  // (`tessera_canvas_draw`: paint the running tree, never re-evaluate). A source that does not evaluate
  // leaves the running tree in the module, so `draw` either is not called at all (no frame owed) or
  // paints the tree that is still there.
  function drawFrame(): void {
    framePending = false;
    if (factory === null || gpu === null || !visible) {
      return;
    }
    const { dpr } = resize();
    options.elements.statDpr.textContent = String(dpr);
    // The layout viewport is the LOGICAL rectangle, and `width`/`height` above are the device-pixel
    // backing store. Passing the backing store as the viewport would lay the tree out at
    // `logical * dpr` and then scale it into the canvas - a 390x844 phone at DPR 2 would lay out at
    // 780x1688 and show the top-left quarter. The C ABI takes both, separately, for this reason.
    const viewport = requestedViewport(options.environment);
    const foldPosture = options.foldPosture();
    const target = source();
    let applied: CanvasApplyReport;
    try {
      // The project is staged first (the module evaluates what is staged, from its entry), then applied
      // with the component, the fold posture and the logical viewport and DPR. Applying and painting are
      // separate steps so a failed apply cannot clear the picture. The author's CSS rides with the staged
      // project: only the sheets the entry's import graph reaches apply.
      stageProject(gpu.module, target);
      applied = JSON.parse(
        callExport(gpu.module, "_tessera_canvas_apply", ["", foldPosture], [
          viewport.width,
          viewport.height,
          dpr,
        ]),
      ) as CanvasApplyReport;
    } catch (error) {
      pane.showFailed(String(error));
      options.onReady(true);
      return;
    }
    // The apply failed: the module kept the running evaluation, so nothing is drawn and the last good
    // frame stays on the canvas. The reason goes to the pane's status line, never to a blank surface.
    if (applied.error !== "") {
      pane.setStatus("source not applied: " + applied.error, "warning");
      options.onReady(true);
      return;
    }
    // The state row follows every successful apply, `changed` or not: a cell that was carried is the
    // point of the edit, and it is the one fact a developer wants to see *while* the picture is still
    // the previous one (a reused apply reports it too).
    pane.renderState(applied.state ?? []);
    // No frame owed: the source is byte-identical to what is painted (a debounce that fired on an
    // unchanged buffer, or a reuse after a DPR-resize that changed nothing). Drawing would repaint
    // identical pixels, which is the idle rule this view exists to honour.
    if (!applied.changed) {
      options.onReady(true);
      return;
    }
    let report: CanvasFrameReport;
    try {
      // One texture-view reference per frame: the module takes ownership and releases it, and the canvas
      // texture it names is only valid for this frame, so it is fetched here and not cached.
      const image = gpu.context.getCurrentTexture().createView();
      const handles = gpu.module.WebGPU;
      if (!handles) {
        throw new Error("the module dropped its WebGPU handle table");
      }
      const viewPtr = handles.importJsTextureView(image);
      // `_tessera_canvas_draw` takes no strings: the tree it paints is the one the apply above left in the
      // module, which already carries the project's stylesheets. `callExport` hands the export
      // `(...pointers, ...numbers)`.
      report = JSON.parse(
        callExport(gpu.module, "_tessera_canvas_draw", [], [
          viewport.width,
          viewport.height,
          viewPtr,
        ]),
      ) as CanvasFrameReport;
    } catch (error) {
      pane.showFailed(String(error));
      options.onReady(true);
      return;
    }
    pane.renderReport(report);
    options.onReady(true);
  }

  // Coalesces any number of change notifications into one frame on the next display refresh.
  function requestFrame(): void {
    if (framePending) {
      return;
    }
    framePending = true;
    globalThis.requestAnimationFrame(drawFrame);
  }

  async function loadModule(): Promise<void> {
    if (factory !== null) {
      return;
    }
    try {
      const imported = (await import(
        /* @vite-ignore */ wasmModuleUrl("tessera_canvas.js")
      )) as { default?: ModuleFactory };
      factory = imported.default ?? null;
      if (typeof factory !== "function") {
        throw new Error("the module did not export a factory");
      }
      options.onReady(true);
    } catch (error) {
      // The module is built by CI and published with the page; a checkout that has not run the
      // `wasm-webgpu` build has no artifacts next to these files, and saying that is more useful than an
      // empty canvas.
      pane.showUnsupported(
        "The WebAssembly canvas module did not load, so this view cannot draw. The module is built " +
          "by CI and published with the page; a checkout that has not run the `wasm-webgpu` build " +
          `has no artifacts next to these files. Detail: ${String(error)}`,
        "module",
      );
    }
  }

  // The device handover, once. Returns true when the view can draw.
  async function startGpu(): Promise<boolean> {
    if (factory === null) {
      return false;
    }
    // **The module is instantiated once, not once per Run.** A Wasm instance owns the GPU device, the
    // running session and the loaded font, so calling the factory again built a fresh instance whose
    // `g_device` is null by definition -- and the reuse branch below then swapped that device-less
    // instance in for the working one. Every Run after the first therefore reported "no WebGPU device"
    // (reproduced in a browser; a Viewport change only made it visible). One instance is also what the
    // rest of this file assumes: `gpu.module`, the font and the session are all per-instance state.
    let module: WasmModule & CanvasModule;
    if (gpu !== null) {
      module = gpu.module;
    } else {
      try {
        module = await factory();
      } catch (error) {
        pane.showFailed(`The preview module failed to instantiate: ${String(error)}`);
        return false;
      }
    }

    try {
      // Both exports are required: `apply` is what an edit and the first automatic load call, `draw`
      // paints the running session, and `set_viewport` is how the toolbar changes the device without
      // re-evaluating (issue #436).
      requireExports(module, [
        "_tessera_canvas_apply",
        "_tessera_canvas_draw",
        "_tessera_canvas_set_viewport",
        "_tessera_webgpu_set_device",
      ]);
    } catch (error) {
      pane.showUnsupported(String(error), "module");
      return false;
    }

    // A device already acquired is reused. A lost device is cleared in the handler below, which is the one
    // case that re-acquires.
    if (gpu === null) {
      const acquired = await acquireDevice();
      if (acquired.error !== undefined) {
        // No navigator.gpu, adapter or device: the browser's side of the handover.
        pane.showUnsupported(acquired.error, "browser");
        return false;
      }
      const handover = handOverDevice(module, acquired.device, acquired.queue, canvas);
      if (handover.error !== undefined) {
        // The browser gave a device; the module could not take it (or the canvas had no context).
        pane.showUnsupported(handover.error, "module");
        return false;
      }
      gpu = { device: acquired.device, queue: acquired.queue, module, context: handover.context };
      // A lost device is the one case that must draw again without a user action: the next Run would
      // otherwise hand the module a device the browser has already dropped.
      void acquired.device.lost.then((info: GPUDeviceLostInfo) => {
        gpu = null;
        pane.showUnsupported(
          `The WebGPU device was lost: ${info.reason || "unknown"}. Open the Canvas view again to ` +
            "reacquire it.",
        );
      });
    }

    resize();
    return true;
  }

  /**
   * Hands the module its font, once per device (issue #387). A failure is **not** fatal: the preview then
   * paints boxes only and the pane says so. The steps live in `canvas-font.ts`.
   */
  async function loadFont(module: WasmModule & CanvasModule): Promise<string | null> {
    return loadFontFor(module as never, (family) => pane.setFontState(family));
  }

  // **Restart app** (issue #444): a cold start. The module's session is released, so the next frame
  // evaluates from nothing, takes no carried state and every cell starts at its initializer -- the same
  // thing a page reload or a launch of the app does. This is the only path that discards state; an
  // edit never does (`apply`).
  async function run(): Promise<void> {
    if (factory === null) {
      return;
    }
    options.onReady(false);
    pane.setStatus("restarting", null);
    // The reasoning behind `hideStates` here and not on an apply: a restart is an explicit, expected
    // cold start, so replacing the pane's content is honest. A live edit must never do it (#444: the
    // canvas cannot blank while typing), which is why `apply` below touches no state but the status.
    const ready = await startGpu();
    if (!ready) {
      options.onReady(true);
      return;
    }
    // The font, after the device: the atlas needs GPU page textures, so `set_font` refuses before a
    // device exists. A failed fetch degrades the preview instead of failing it.
    if (gpu !== null) {
      const fontError = await loadFont(gpu.module);
      if (fontError !== null) {
        // Named, not silent: a designer must be able to tell "this source has no text" from "the font
        // did not load", and the status line is where the pane already reports that class of fact.
        pane.setStatus("font unavailable; text is not drawn", "warning");
        pane.setFontState(null, fontError);
      }
    }
    if (gpu !== null) {
      // The cold start itself: `tessera_canvas_reset` drops the running evaluation, so the draw that
      // follows evaluates the source afresh with nothing carried. It returns a small document (the
      // released session's frame count) rather than void, because `callExport` reads every export as a
      // returned document -- a void export would surface as "returned no document", a crash-shaped
      // message for a successful restart.
      callExport(gpu.module, "_tessera_canvas_reset");
      // The state row belongs to the evaluation that was just released, so it goes with it: after a
      // restart the count is whatever the initializer says, and showing the old value until the next
      // frame lands would be the one stale reading on the page.
      pane.renderState([]);
    }
    requestFrame();
  }

  /**
   * Applies an edit without resetting anything (issue #444).
   *
   * This is the hot path the debounced trigger calls ~150 ms after a keystroke, and the same thing
   * `Ctrl+Enter` calls immediately. It must never take a destructive action: the frame it requests
   * either paints the re-evaluated tree (state carried by the engine's own rule) or, when the source
   * does not compile, paints nothing at all and leaves the last good picture plus a status sentence.
   *
   * It also must not *cost* a frame when nothing changed, which is the module's answer (`changed`),
   * not a decision made here.
   */
  async function apply(): Promise<void> {
    if (factory === null) {
      return;
    }
    // The device is acquired here, lazily, on the first apply -- never on page load. A session that has
    // not drawn yet has no device, and an apply is the first thing that needs one.
    const ready = await startGpu();
    if (!ready) {
      return;
    }
    if (gpu !== null && lastDpr === 0) {
      // The font is handed over once per device, and the first apply is where a device first exists.
      // `lastDpr === 0` is exactly "this session has not resized yet", i.e. no frame has been drawn, so
      // this runs on the first apply and never again -- and never on a restart, which the `run` path
      // above has already handled.
      const fontError = await loadFont(gpu.module);
      if (fontError !== null) {
        pane.setFontState(null, fontError);
      }
    }
    requestFrame();
  }

  // A device-pixel-ratio change (a window dragged to another monitor, a browser zoom) changes the backing
  // store, which is a real reason to draw again. A resize alone does not: the CSS box follows the toolbar,
  // and the toolbar has its own change hook.
  globalThis.addEventListener("resize", () => {
    const dpr = globalThis.devicePixelRatio > 0 ? globalThis.devicePixelRatio : 1;
    if (gpu !== null && visible && dpr !== lastDpr) {
      requestFrame();
    }
  });

  // A hidden tab draws nothing; becoming visible again is the one event that restores an on-demand canvas
  // that was not painted while it was hidden.
  document.addEventListener("visibilitychange", () => {
    if (gpu !== null && visible && !document.hidden) {
      requestFrame();
    }
  });

  return {
    id: "canvas",
    pane,
    /** First open of the view: decide the unsupported state and import the module (no device yet). */
    load: async () => {
      visible = true;
      if (!navigator.gpu) {
        // Decided from the API's presence, which needs no download: a browser without WebGPU is told so
        // without paying for a module it cannot use.
        pane.showUnsupported(
          "This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ " +
            "and Firefox 141+ (on supported hardware). The Scene tab needs no GPU at all.",
        );
        return;
      }
      await loadModule();
      if (factory !== null) {
        pane.setStatus("ready", null);
      }
    },
    /** The **Restart app** button's action: an explicit cold start, state reset. */
    run,
    /**
     * An edit was applied (issue #444): the debounced trigger, `Ctrl+Enter`, and the first load.
     *
     * It is the same entry point for all three on purpose. The first automatic apply is what acquires
     * the device, so "the preview starts itself once the module and the device are ready" costs no
     * separate code path -- and a page whose Canvas tab has never been opened has no session to apply
     * to, so a visitor who never looks at Canvas still takes no GPU.
     */
    apply,
    /** "Load sample": the canvas draws whatever the shared editor holds, so only a live device redraws. */
    loadSample: () => {
      if (gpu !== null && visible) {
        requestFrame();
      }
    },
    /** The pane became (in)visible: stop drawing while hidden, redraw once when shown again. */
    setVisible: (shown: boolean) => {
      visible = shown;
      if (shown && gpu !== null) {
        requestFrame();
      }
    },
    /**
     * The toolbar moved: on this view the viewport, the DPR and the fold posture are what the canvas is
     * sized and laid out for, and the layout pass happens inside the engine call this makes (issues #408
     * and #436). **A viewport change is not a reason to re-evaluate.** `_tessera_canvas_set_viewport`
     * re-lays the running evaluation out in place — no re-run, `@media` re-resolved only when the size
     * crosses a breakpoint — so `state::count`, hover and focus survive a preset or orientation swap.
     */
    onEnvironmentChange: () => {
      if (gpu === null || !visible) {
        return;
      }
      resize();
      const viewport = requestedViewport(options.environment);
      try {
        const report = JSON.parse(
          callExport(gpu.module, "_tessera_canvas_set_viewport", [], [
            viewport.width,
            viewport.height,
            viewport.dpr,
          ]),
        ) as CanvasViewportReport;
        if (report.error !== "") {
          // A refused viewport (a degenerate size) is named, and the picture already there stays.
          pane.setStatus("viewport not applied: " + report.error, "warning");
          return;
        }
      } catch (error) {
        pane.showFailed(String(error));
        return;
      }
      requestFrame();
    },
    /** A gallery pick changes what would be drawn; redraw only if a device already exists. */
    onSample: () => {
      if (gpu !== null && visible) {
        requestFrame();
      }
    },
  };
}
