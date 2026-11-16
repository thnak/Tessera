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
//  1. **Render on demand.** A static scene draws once. A frame is requested only when something changed: a
//     Run, a device-pixel-ratio change, a toolbar change, or the pane becoming visible again. There is no
//     `requestAnimationFrame` loop spinning at 60 Hz -- the repository's idle rule (a static UI must present
//     nothing) applies to the browser page too. The frame callback is gated on the view being visible, so a
//     hidden pane never draws.
//
//  2. **The unsupported path is a state, not a blank canvas.** `canvas-device.ts` turns every way of
//     failing -- no `navigator.gpu`, no adapter, `requestDevice()` rejecting, a module without the handover
//     export -- into a sentence the pane shows with a pointer at the Scene tab.
//
//  3. **The device handover is explicit and happens once.** A device is acquired on the first Run and
//     reused; re-running the handover per Run would leak a device per click. A *lost* device is the one case
//     that re-acquires, and it says so.
//
//  4. **The module and the device are lazy.** The module is imported, and the device acquired, only when the
//     Canvas view is first opened and Run is first pressed there. The unsupported state is decided from
//     `navigator.gpu`'s presence, which needs no async work and no download, and is shown the moment the tab
//     opens -- so a browser without WebGPU never downloads `tessera_canvas.wasm`.
//
// What the view does NOT claim: the pixels are the module's, but the comparison against the lavapipe golden
// is a CI/browser-lane job (`scripts/ci/scene_frame_parity.py`). This module cannot read pixels back
// synchronously, which is why no report it returns claims `pixelsAvailable`.

import { wasmModuleUrl } from "../lib/module-url";
import type { StatusReporter, ViewSession } from "../lib/views";
import { callExport, requireExports, type WasmModule } from "../lib/wasm-call";
import { acquireDevice, handOverDevice, type CanvasModule } from "./canvas-device";
import {
  requestedViewport,
  sizeToDevicePixels,
  type CanvasFrameReport,
} from "./canvas-frame";
import { createCanvasPane, type CanvasPane, type CanvasPaneElements } from "./canvas-pane";

export type { CanvasFrameReport };

/** The module factory `tessera_canvas.js` exports. */
type ModuleFactory = () => Promise<WasmModule & CanvasModule>;

export interface CanvasSessionOptions {
  elements: CanvasPaneElements;
  source: HTMLTextAreaElement;
  setStatus: StatusReporter;
  /** The toolbar's current `{width, height, dpr}`; a canvas run must not be stuck at 320x240. */
  environment: () => { width: number; height: number; dpr: number };
  /**
   * The toolbar's simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` |
   * `tabletop`. It is a layout input, so a change draws again rather than waiting for a Run.
   */
  foldPosture: () => string;
  /** The source path the current sample is previewed under. */
  file: () => string;
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
      // Pointers first, then the numbers: `callExport` marshals the strings and hands the export
      // `(...pointers, ...numbers)`, which is the shape every export in this page uses.
      report = JSON.parse(
        callExport(
          gpu.module,
          "_tessera_canvas_frame",
          [source.value, options.file(), "", foldPosture],
          [viewport.width, viewport.height, dpr, viewPtr],
        ),
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
      );
    }
  }

  // The device handover, once. Returns true when the view can draw.
  async function startGpu(): Promise<boolean> {
    if (factory === null) {
      return false;
    }
    let module: WasmModule & CanvasModule;
    try {
      module = await factory();
    } catch (error) {
      pane.showFailed(`The preview module failed to instantiate: ${String(error)}`);
      return false;
    }

    try {
      requireExports(module, ["_tessera_canvas_frame", "_tessera_webgpu_set_device"]);
    } catch (error) {
      pane.showUnsupported(String(error));
      return false;
    }

    // A device already acquired is reused. A lost device is cleared in the handler below, which is the one
    // case that re-acquires.
    if (gpu === null) {
      const acquired = await acquireDevice();
      if (acquired.error !== undefined) {
        pane.showUnsupported(acquired.error);
        return false;
      }
      const handover = handOverDevice(module, acquired.device, acquired.queue, canvas);
      if (handover.error !== undefined) {
        pane.showUnsupported(handover.error);
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
    } else {
      gpu.module = module;
    }

    resize();
    return true;
  }

  async function run(): Promise<void> {
    if (factory === null) {
      return;
    }
    options.onReady(false);
    pane.setStatus("running", null);
    pane.hideStates();
    const ready = await startGpu();
    if (!ready) {
      options.onReady(true);
      return;
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
        pane.setStatus("not started; press Run", null);
      }
    },
    /** The Run button's action. */
    run,
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
     * sized and laid out for. One frame is requested; the posture's layout pass happens inside the
     * engine call that frame makes, so the picture is never a frame behind the control (issue #408).
     */
    onEnvironmentChange: () => {
      if (gpu !== null && visible) {
        resize();
        requestFrame();
      }
    },
    /** A gallery pick changes what would be drawn; redraw only if a device already exists. */
    onSample: () => {
      if (gpu !== null && visible) {
        requestFrame();
      }
    },
  };
}
