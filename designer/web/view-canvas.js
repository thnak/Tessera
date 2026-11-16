// SPDX-License-Identifier: MIT

// The canvas view of the merged studio page (issue #392; the view is P3, issue #167).
//
// It paints the evaluated scene through WebGPU. This was `canvas.js` on its own page; the merge moved
// the page chrome into `studio-page.js`/`shell.js`/`studio.js` and, more importantly, made the *module
// and the device* lazy: this view's module is imported and its WebGPU device is acquired only when the
// Canvas view is first opened, and the device not before the first Run here. The old page loaded its
// module eagerly "so an unsupported browser is told so before the first Run" -- a merged page cannot
// afford that, because every visitor would then download the canvas module for a page that may never
// show a canvas. The unsupported state is instead decided from `navigator.gpu`'s presence, which needs
// no async work and no download, and is shown the moment the tab opens.
//
// Three things this module is careful about, because each is a requirement rather than a detail:
//
//  1. **Render on demand.** A static scene draws once. A frame is requested only when something
//     changed: a Run, a device-pixel-ratio change, a toolbar change, or the pane becoming visible
//     again. There is no `requestAnimationFrame` loop that keeps spinning at 60 Hz -- the repository's
//     idle rule (a static UI must present nothing) applies to the browser page too. The frame callback
//     is gated on the view being visible, so a hidden pane never draws.
//
//  2. **The unsupported path is a state, not a blank canvas.** `canvas-device.js` turns every way of
//     failing -- no `navigator.gpu`, no adapter, `requestDevice()` rejecting, a module without the
//     handover export -- into a sentence the pane shows with a pointer at the Scene tab. The states
//     themselves are `canvas-pane.js`, so this file is the draw path.
//
//  3. **The device handover is explicit and happens once.** A device is acquired on the first Run and
//     reused; re-running the handover per Run would leak a device per click. A *lost* device is the one
//     case that re-acquires, and it says so.
//
// What the view does NOT claim: the pixels are the module's, but the comparison against the lavapipe
// golden is a CI/browser-lane job (scripts/ci/scene_frame_parity.py). This module cannot read pixels
// back synchronously, which is why `pixelsAvailable` is false in every report it returns.

import { callExport, requireExports } from './wasm-call.js';
import { acquireDevice, handOverDevice } from './canvas-device.js';
import { canvasPane } from './canvas-pane.js';

const MODULE_URL = './tessera_canvas.js';

// The logical viewport of the preview, matching the Wasm export's fixed 320x240 (and the native CLI's
// default). A moved toolbar overrides it; the canvas is that size times the device pixel ratio.
const VIEWPORT_WIDTH = 320;
const VIEWPORT_HEIGHT = 240;

/**
 * Mounts the canvas view.
 *
 * `options.source` is the page's shared textarea; `options.viewport()` returns the toolbar's current
 * `{width, height, dpr}` so a canvas run is not stuck at 320x240 while the toolbar says otherwise.
 */
export function mountCanvas(options) {
  const source = options.source;
  const setEnabled = options.setEnabled || (() => {});
  const pane = canvasPane();

  // The module factory, or null before it loads. `gpu` holds the device the page acquired.
  let factory = null;
  let gpu = null;
  // Set when a frame has been requested and not yet drawn, so a burst of changes makes one frame.
  let framePending = false;
  // The dpr the canvas was last sized for; a change resizes it (a real, drawable event).
  let lastDpr = 0;
  // Cleared while the view is hidden, so a hidden pane never draws.
  let visible = false;

  function currentViewport() {
    const requested = options.viewport ? options.viewport() : null;
    if (!requested || !requested.width || !requested.height) {
      return { width: VIEWPORT_WIDTH, height: VIEWPORT_HEIGHT, dpr: 1 };
    }
    return requested;
  }

  // Sizes the backing store to the requested viewport and device pixel ratio and returns the physical
  // extent. A canvas whose backing store is its CSS size is blurry on every high-density display, and
  // a preview judged on blurry pixels would not be a preview.
  function resizeToDevicePixels() {
    const requested = currentViewport();
    const dpr = requested.dpr && requested.dpr > 0 ? requested.dpr : 1;
    const width = Math.round(requested.width * dpr);
    const height = Math.round(requested.height * dpr);
    if (pane.canvas.width !== width || pane.canvas.height !== height) {
      pane.canvas.width = width;
      pane.canvas.height = height;
    }
    pane.canvas.style.width = `${requested.width}px`;
    pane.canvas.style.height = `${requested.height}px`;
    lastDpr = dpr;
    return { dpr, width, height };
  }

  // Draws the current source. Called from a `requestAnimationFrame` callback, never from the input
  // handler, so a fast typist cannot make the paint path reentrant and the canvas updates in step with
  // the display.
  function drawFrame() {
    framePending = false;
    if (factory === null || gpu === null || !visible) {
      return;
    }
    const { dpr, width, height } = resizeToDevicePixels();
    pane.statDpr.textContent = String(dpr);
    let report;
    try {
      const file = options.file ? options.file() : 'ui/Preview.tsx';
      // One texture-view reference per frame: the module takes ownership and releases it, and the
      // canvas texture it names is only valid for this frame, so it is fetched here and not cached.
      const viewPtr = gpu.module.WebGPU.importJsTextureView(
        gpu.context.getCurrentTexture().createView(),
      );
      report = JSON.parse(
        callExport(gpu.module, '_tessera_canvas_frame', [source.value, file, ''], [
          width,
          height,
          dpr,
          viewPtr,
        ]),
      );
    } catch (error) {
      pane.showFailed(String(error));
      setEnabled('run', true);
      return;
    }
    pane.renderReport(report);
    setEnabled('run', true);
  }

  // Coalesces any number of change notifications into one frame on the next display refresh.
  function requestFrame() {
    if (framePending) {
      return;
    }
    framePending = true;
    window.requestAnimationFrame(drawFrame);
  }

  async function loadModule() {
    if (factory !== null) {
      return;
    }
    try {
      const imported = await import(MODULE_URL);
      factory = imported.default;
      if (typeof factory !== 'function') {
        throw new Error('the module did not export a factory');
      }
      setEnabled('run', true);
    } catch (error) {
      // The module is built by CI and published with the page; a checkout that has not run the
      // `wasm-webgpu` build has no artifacts next to these files, and saying that is more useful than
      // an empty canvas.
      pane.showUnsupported(
        'The WebAssembly canvas module did not load, so this view cannot draw. The module is built ' +
          'by CI and published with the page; a checkout that has not run the `wasm-webgpu` build ' +
          `has no artifacts next to these files. Detail: ${String(error)}`,
      );
    }
  }

  // The device handover, once. Returns true when the view can draw.
  async function startGpu() {
    let module = null;
    try {
      module = await factory();
    } catch (error) {
      pane.showFailed(`The preview module failed to instantiate: ${String(error)}`);
      return false;
    }

    try {
      requireExports(module, ['_tessera_canvas_frame', '_tessera_webgpu_set_device']);
    } catch (error) {
      pane.showUnsupported(String(error));
      return false;
    }

    // A device already acquired is reused. A lost device is cleared in the handler below, which is the
    // one case that re-acquires.
    if (gpu === null) {
      const acquired = await acquireDevice();
      if (acquired.error) {
        pane.showUnsupported(acquired.error);
        return false;
      }
      const handover = handOverDevice(module, acquired.device, acquired.queue, pane.canvas);
      if (handover.error) {
        pane.showUnsupported(handover.error);
        return false;
      }
      gpu = { device: acquired.device, queue: acquired.queue, module, context: handover.context };
      // A lost device is the one case that must draw again without a user action: the next Run would
      // otherwise hand the module a device the browser has already dropped.
      acquired.device.lost.then((info) => {
        gpu = null;
        pane.showUnsupported(
          `The WebGPU device was lost: ${info.reason || 'unknown'}. Open the Canvas view again to ` +
            'reacquire it.',
        );
      });
    } else {
      gpu.module = module;
    }

    resizeToDevicePixels();
    return true;
  }

  async function run() {
    if (factory === null) {
      return;
    }
    setEnabled('run', false);
    pane.setStatus('running', null);
    pane.hideStates();
    const ready = await startGpu();
    if (!ready) {
      setEnabled('run', true);
      return;
    }
    requestFrame();
  }

  // A device-pixel-ratio change (a window dragged to another monitor, a browser zoom) changes the
  // backing store, which is a real reason to draw again. A resize alone does not: the CSS box follows
  // the toolbar, and the toolbar has its own change hook.
  window.addEventListener('resize', () => {
    const dpr = window.devicePixelRatio && window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
    if (gpu !== null && visible && dpr !== lastDpr) {
      requestFrame();
    }
  });

  // A hidden tab draws nothing; becoming visible again is the one event that restores an on-demand
  // canvas that was not painted while it was hidden.
  document.addEventListener('visibilitychange', () => {
    if (gpu !== null && visible && !document.hidden) {
      requestFrame();
    }
  });

  return {
    id: 'canvas',
    /** First open of the view: decide the unsupported state and import the module (no device yet). */
    load: async () => {
      visible = true;
      if (!navigator.gpu) {
        // Decided from the API's presence, which needs no download: a browser without WebGPU is told
        // so without paying for a module it cannot use.
        pane.showUnsupported(
          'This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ ' +
            'and Firefox 141+ (on supported hardware). The Scene tab needs no GPU at all.',
        );
        return;
      }
      await loadModule();
      if (factory !== null) {
        pane.setStatus('not started; press Run', null);
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
    setVisible: (shown) => {
      visible = shown;
      if (shown && gpu !== null) {
        requestFrame();
      }
    },
    /** The toolbar moved: on this view the viewport and DPR are what the canvas is sized for. */
    onEnvironmentChange: () => {
      if (gpu !== null && visible) {
        resizeToDevicePixels();
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
