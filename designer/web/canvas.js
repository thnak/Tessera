// SPDX-License-Identifier: MIT

// The WebGPU canvas preview page (issue #167, P3 of the design-time preview). Vanilla ES module: no
// framework, no bundler, no build step, so `canvas.html` + `canvas.js` + `app.css` + the Wasm module
// are the whole page.
//
// Three things this file is careful about, because each is a requirement rather than a detail:
//
//  1. **Render on demand.** A static scene draws once. A frame is requested only when something
//     changed: a Run, a device-pixel-ratio change, or the tab becoming visible again. There is no
//     `requestAnimationFrame` loop that keeps spinning at 60 Hz - the repository's idle rule (a
//     static UI must present nothing) applies to the browser page too, and a 60 Hz canvas would
//     drain a laptop battery on a page nobody is touching. `requestAnimationFrame` is still the
//     scheduling primitive: it is what makes the draw happen once per display refresh *when* there
//     is something to draw, and it is how a burst of changes coalesces into one frame.
//
//  2. **The unsupported path is a state, not a blank canvas.** If `navigator.gpu` is missing, or
//     `requestAdapter()` returns null, or `requestDevice()` rejects, the page shows a named reason
//     and links the P2 scene page, which needs no GPU. It never leaves an empty canvas on screen and
//     it never throws into the console expecting the reader to notice.
//
//  3. **The device handover is explicit.** The module cannot request a device (that is async
//     JavaScript), so this file does the dance and calls `tessera_webgpu_set_device(device, queue)`
//     once. If that export is missing, the page says so instead of silently drawing nothing.
//
// What the page does NOT claim: the pixels are the module's, but the comparison against the lavapipe
// golden is a CI/browser-lane job (scripts/ci/scene_frame_parity.py). This module cannot read pixels
// back synchronously, which is why `pixelsAvailable` is false in every report it returns.

const MODULE_URL = './tessera_canvas.js';

// The same sample the P2 page shows: a static tree the preview evaluates, so the first Run draws a
// real scene instead of an unsupported-construct report.
const SAMPLE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

// The logical viewport of the preview, matching the Wasm export's fixed 320x240 (and the native
// CLI's default). The canvas is that size multiplied by the device pixel ratio.
const VIEWPORT_WIDTH = 320;
const VIEWPORT_HEIGHT = 240;

// The canvas format the page configures and the module paints in (wasm_canvas_entry.cpp's
// kCanvasFormat). Keep the two in step: a pipeline built for another format is rejected at draw.
const CANVAS_FORMAT = 'rgba8unorm';

const source = document.getElementById('source');
const runButton = document.getElementById('run');
const sampleButton = document.getElementById('load-sample');
const status = document.getElementById('status');
const unsupportedBox = document.getElementById('unsupported');
const unsupportedHeading = document.getElementById('unsupported-heading');
const unsupportedBody = document.getElementById('unsupported-body');
const canvasFrame = document.getElementById('canvas-frame');
const canvas = document.getElementById('canvas');
const failedBox = document.getElementById('failed');
const failedBody = document.getElementById('failed-body');
const frameStats = document.getElementById('frame-stats');
const statFrame = document.getElementById('stat-frame');
const statDraws = document.getElementById('stat-draws');
const statVertices = document.getElementById('stat-vertices');
const statNodes = document.getElementById('stat-nodes');
const statDpr = document.getElementById('stat-dpr');
const buildId = document.getElementById('build-id');

// The module factory, or null before it loads. `gpu` holds the device the page acquired.
let factory = null;
let gpu = null;
// Set when a frame has been requested and not yet drawn, so a burst of changes makes one frame.
let framePending = false;
// The dpr the canvas was last sized for; a change resizes it (a real, drawable event).
let lastDpr = 0;

function setStatus(text, kind) {
  status.textContent = text;
  status.className = 'status' + (kind ? ' ' + kind : '');
}

function showUnsupported(reason) {
  unsupportedHeading.textContent = 'This browser cannot show the canvas preview';
  unsupportedBody.textContent = reason;
  unsupportedBox.hidden = false;
  canvasFrame.hidden = true;
  frameStats.hidden = true;
  setStatus('WebGPU unavailable', 'warning');
}

function showFailed(message) {
  failedBody.textContent = message;
  failedBox.hidden = false;
  setStatus('failed', 'warning');
}

function hideStates() {
  unsupportedBox.hidden = true;
  failedBox.hidden = true;
}

// Sizes the backing store to the device pixel ratio and returns the physical extent. A canvas whose
// backing store is its CSS size is blurry on every high-density display, and a preview judged on
// blurry pixels would not be a preview.
function resizeToDevicePixels() {
  const dpr = window.devicePixelRatio && window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
  const width = Math.round(VIEWPORT_WIDTH * dpr);
  const height = Math.round(VIEWPORT_HEIGHT * dpr);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  canvas.style.width = `${VIEWPORT_WIDTH}px`;
  canvas.style.height = `${VIEWPORT_HEIGHT}px`;
  lastDpr = dpr;
  return { dpr, width, height };
}

// Acquires the adapter and device. Every failure path returns a string a person can read, because
// the alternative is the console-only "GPUAdapter is null" that tells a designer nothing.
async function acquireDevice() {
  if (!navigator.gpu) {
    return { error: 'This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ and Firefox 141+ (on supported hardware).' };
  }
  let adapter;
  try {
    adapter = await navigator.gpu.requestAdapter();
  } catch (error) {
    return { error: `requestAdapter() failed: ${String(error)}` };
  }
  if (!adapter) {
    return {
      error:
        'No WebGPU adapter is available. This usually means the GPU or its driver is blocklisted, ' +
        'or the page is in a browser context that forbids GPU access.',
    };
  }
  try {
    const device = await adapter.requestDevice();
    if (!device) {
      return { error: 'requestDevice() returned no device.' };
    }
    return { device, queue: device.queue };
  } catch (error) {
    return { error: `requestDevice() failed: ${String(error)}` };
  }
}

// Draws the current source. Called from a `requestAnimationFrame` callback, never from the input
// handler, so a fast typist cannot make the paint path reentrant and the canvas updates in step with
// the display.
function drawFrame() {
  framePending = false;
  if (factory === null || gpu === null) {
    return;
  }
  const { dpr, width, height } = resizeToDevicePixels();
  statDpr.textContent = String(dpr);
  let module = null;
  let report;
  try {
    module = gpu.module;
    const text = source.value;
    const file = 'ui/Preview.tsx';

    // The C ABI takes bytes in the module heap and owns the report it returns: `tessera_free` is the
    // documented matching release, so every allocation below is undone.
    const textLength = module.lengthBytesUTF8(text) + 1;
    const textPtr = module._malloc(textLength);
    const fileLength = module.lengthBytesUTF8(file) + 1;
    const filePtr = module._malloc(fileLength);
    const componentPtr = module._malloc(1);
    let json;
    try {
      module.stringToUTF8(text, textPtr, textLength);
      module.stringToUTF8(file, filePtr, fileLength);
      module.stringToUTF8('', componentPtr, 1);
      // One texture-view reference per frame: the module takes ownership and releases it, and the
      // canvas texture it names is only valid for this frame, so it is fetched here and not cached.
      const viewPtr = module.WebGPU.importJsTextureView(gpu.context.getCurrentTexture().createView());
      const resultPtr = module._tessera_canvas_frame(
        textPtr,
        filePtr,
        componentPtr,
        width,
        height,
        dpr,
        viewPtr
      );
      if (!resultPtr) {
        throw new Error('tessera_canvas_frame returned null');
      }
      try {
        json = module.UTF8ToString(resultPtr);
      } finally {
        module._tessera_free(resultPtr);
      }
    } finally {
      module._free(textPtr);
      module._free(filePtr);
      module._free(componentPtr);
    }
    report = JSON.parse(json);
  } catch (error) {
    showFailed(String(error));
    runButton.disabled = false;
    return;
  }

  if (report.hasDevice !== true) {
    // The module could not wrap the handles the page handed it; the page is honest about that
    // rather than showing a canvas that will never paint.
    showUnsupported(report.error || 'The preview module got no WebGPU device.');
    runButton.disabled = false;
    return;
  }

  hideStates();
  canvasFrame.hidden = false;
  frameStats.hidden = false;
  statFrame.textContent = report.width + 'x' + report.height;
  statDraws.textContent = String(report.drawCalls);
  statVertices.textContent = String(report.vertices);
  statNodes.textContent = String(report.nodes);
  if (report.error) {
    showFailed(report.error);
  } else if (report.drawCalls === 0) {
    // A scene that produced no draws is not a crash, but it is also not a picture: say so, the way
    // the P2 page reports an empty document instead of an empty list.
    setStatus('no draw calls (the source produced no painted boxes)', 'warning');
  } else {
    setStatus('drew ' + report.drawCalls + ' draw call(s)', 'ok');
  }
  runButton.disabled = false;
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
  try {
    const imported = await import(MODULE_URL);
    factory = imported.default;
    runButton.disabled = false;
  } catch (error) {
    // The module is built by CI and published with the page; a checkout that has not run the
    // `wasm-webgpu` build has no artifacts next to these files, and saying that is more useful than
    // an empty canvas.
    showUnsupported(
      'The WebAssembly canvas module did not load, so this page cannot draw. The module is built by ' +
        'CI and published with the page; a checkout that has not run the `wasm-webgpu` build has no ' +
        `artifacts next to these files. Detail: ${String(error)}`
    );
  }
}

async function run() {
  if (factory === null) {
    return;
  }
  runButton.disabled = true;
  setStatus('running', null);
  hideStates();
  let module = null;
  try {
    module = await factory();
  } catch (error) {
    showFailed(`The preview module failed to instantiate: ${String(error)}`);
    runButton.disabled = false;
    return;
  }

  if (typeof module._tessera_canvas_frame !== 'function') {
    showUnsupported(
      'The module this page loaded has no `tessera_canvas_frame` export, so it is not the canvas ' +
        'build. The P2 scene module answers a different question and the pages load different files.'
    );
    runButton.disabled = false;
    return;
  }

  // The device is acquired once and reused; a page that re-ran the handover per Run would leak a
  // device per click.
  const acquired = await acquireDevice();
  if (acquired.error) {
    showUnsupported(acquired.error);
    runButton.disabled = false;
    return;
  }

  // The export exists only in the canvas build; a module without it would draw nothing.
  if (typeof module._tessera_webgpu_set_device !== 'function') {
    showUnsupported(
      'The module this page loaded cannot accept a WebGPU device (no `tessera_webgpu_set_device`).'
    );
    runButton.disabled = false;
    return;
  }

  // The C ABI takes Emscripten WebGPU handles, not JS objects (a JS object coerces to 0, which the
  // module reads as a null handle and answers "no WebGPU device"). The module exports the port's
  // handle table for exactly this import.
  if (!module.WebGPU || typeof module.WebGPU.importJsDevice !== 'function') {
    showUnsupported(
      'The module this page loaded does not export the WebGPU handle table (`WebGPU.importJsDevice`), ' +
        'so it cannot take the browser device. Rebuild the canvas module with the current link options.'
    );
    runButton.disabled = false;
    return;
  }
  const devicePtr = module.WebGPU.importJsDevice(acquired.device);
  const queuePtr = module.WebGPU.importJsQueue(acquired.queue);
  const handover = module._tessera_webgpu_set_device(devicePtr, queuePtr);
  if (handover !== 1) {
    showUnsupported(
      handover === 0
        ? 'The module received a null WebGPU handle from the page.'
        : 'The module could not wrap the WebGPU device the page handed it.'
    );
    runButton.disabled = false;
    return;
  }
  // The canvas surface is the page's to configure (the RHI never owns it): RGBA8 is one of the two
  // formats every WebGPU canvas must support, and it is the format the module paints in. Opaque,
  // because the preview clears to the app background and a transparent canvas would show the page.
  const context = canvas.getContext('webgpu');
  if (!context) {
    showUnsupported('The canvas could not create a WebGPU context, so the frame has nowhere to be shown.');
    runButton.disabled = false;
    return;
  }
  context.configure({ device: acquired.device, format: CANVAS_FORMAT, alphaMode: 'opaque' });
  gpu = { device: acquired.device, queue: acquired.queue, module, context };
  // A lost device is the one case that must draw again without a user action: the next Run would
  // otherwise hand the module a device the browser has already dropped.
  acquired.device.lost.then((info) => {
    gpu = null;
    showUnsupported(`The WebGPU device was lost: ${info.reason || 'unknown'}. Reload the page to reacquire it.`);
  });

  resizeToDevicePixels();
  requestFrame();
}

runButton.addEventListener('click', () => {
  void run();
});
sampleButton.addEventListener('click', () => {
  source.value = SAMPLE;
  // Loading the sample changes what would be drawn, so the canvas must show it - but only if a
  // device already exists (before the first Run there is nothing to redraw).
  if (gpu !== null) {
    requestFrame();
  }
});
source.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault();
    void run();
  }
});

// A device-pixel-ratio change (a window dragged to another monitor, a browser zoom) changes the
// backing store, which is a real reason to draw again. A resize alone does not: the CSS box is
// fixed, so nothing about the scene changed.
window.addEventListener('resize', () => {
  const dpr = window.devicePixelRatio && window.devicePixelRatio > 0 ? window.devicePixelRatio : 1;
  if (gpu !== null && dpr !== lastDpr) {
    requestFrame();
  }
});

// A hidden tab draws nothing; becoming visible again is the one event that restores an on-demand
// canvas that was not painted while it was hidden.
document.addEventListener('visibilitychange', () => {
  if (gpu !== null && !document.hidden) {
    requestFrame();
  }
});

// `build-id.txt` is written by the publish job; a local checkout has none, and the field says so
// rather than inventing one.
fetch('./build-id.txt')
  .then((response) => (response.ok ? response.text() : Promise.reject(new Error('absent'))))
  .then((text) => {
    const id = text.trim();
    buildId.textContent = 'build: ' + (id || 'unknown');
  })
  .catch(() => {
    buildId.textContent = 'build: local (no build-id.txt)';
    buildId.title = 'The publish job writes build-id.txt; a local checkout has none.';
  });

source.value = SAMPLE;
runButton.disabled = true;
// The module is loaded eagerly so an unsupported browser is told so before the first Run, but no
// device is requested until the user asks for a frame: acquiring a GPU on page load would take the
// whole machine's GPU for a page that may never be used.
void loadModule();

// The unsupported state is decided from the API's presence, which needs no async work, so it shows on
// load rather than after a click.
if (!navigator.gpu) {
  showUnsupported(
    'This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ and ' +
      'Firefox 141+ (on supported hardware).'
  );
  runButton.disabled = true;
}
