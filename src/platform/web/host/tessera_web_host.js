// SPDX-License-Identifier: MIT
// Browser host glue for a Tessera web app (ADR-046). Pure ES module, no dependencies, no build step.
//
// It does three things and nothing else: checks WebGPU and shows the "unsupported browser" page when
// it is missing (decision 4), flattens DOM events into the numbers `src/platform/web/web_exports.cpp`
// takes (decision 3), and turns the C++ host's "request a frame" into one requestAnimationFrame
// (decision 5). The pure helpers are exported so tests/web/host_glue_tests.mjs runs them in Node.

// Bit layout shared with web_input_translation.hpp (kMod*).
export const MOD_SHIFT = 1, MOD_CONTROL = 2, MOD_ALT = 4, MOD_META = 8, MOD_CAPS = 16, MOD_NUM = 32;

export function modifierBits(event) {
  const state = (name) => (typeof event.getModifierState === 'function' ? event.getModifierState(name) : false);
  let bits = 0;
  if (event.shiftKey) bits |= MOD_SHIFT;
  if (event.ctrlKey) bits |= MOD_CONTROL;
  if (event.altKey) bits |= MOD_ALT;
  if (event.metaKey) bits |= MOD_META;
  if (state('CapsLock')) bits |= MOD_CAPS;
  if (state('NumLock')) bits |= MOD_NUM;
  return bits;
}

// DomPointerPhase / DomPointerType in web_input_translation.hpp.
export const PHASE = { pointerdown: 0, pointermove: 1, pointerup: 2, pointercancel: 3 };
export const POINTER_TYPE = { mouse: 0, touch: 1, pen: 2 };

export function pointerTypeCode(name) {
  return Object.prototype.hasOwnProperty.call(POINTER_TYPE, name) ? POINTER_TYPE[name] : 3;
}

export function webgpuAvailable(nav) {
  return Boolean(nav && nav.gpu && typeof nav.gpu.requestAdapter === 'function');
}

// Canvas-relative CSS pixels: the logical unit InputEvent uses, so the device pixel ratio never
// reaches the engine through input.
export function canvasPoint(event, rect) {
  return [event.clientX - rect.left, event.clientY - rect.top];
}

// On-demand requestAnimationFrame. `requestFrame` is idempotent while a callback is pending and
// `wakeAfter` arms at most one timer, so a parked page holds neither: the "no frame callback when
// idle" rule of ADR-046 item 5 lives here and in FrameCallbackScheduler.
export function createFrameBridge({ raf, caf, setTimer, clearTimer, onFrame }) {
  let frameHandle = null;
  let timerHandle = null;
  const bridge = {
    requestFrame() {
      if (frameHandle !== null) return;
      frameHandle = raf((timestamp) => {
        frameHandle = null;
        onFrame(timestamp);
      });
    },
    wakeAfter(milliseconds) {
      if (timerHandle !== null) clearTimer(timerHandle);
      timerHandle = setTimer(() => {
        timerHandle = null;
        bridge.requestFrame();
      }, Math.max(0, milliseconds));
    },
    cancel() {
      if (frameHandle !== null) caf(frameHandle);
      if (timerHandle !== null) clearTimer(timerHandle);
      frameHandle = null;
      timerHandle = null;
    },
    pending() {
      return frameHandle !== null;
    },
  };
  return bridge;
}

export function showUnsupported(doc) {
  const page = doc.getElementById('unsupported');
  const canvas = doc.getElementById('tessera-canvas');
  if (canvas) canvas.hidden = true;
  if (page) page.hidden = false;
}

// Attaches the DOM listeners that feed the C++ host. Returns a function that removes them.
export function attachInput({ canvas, exports, win, doc }) {
  const send = [];
  const on = (target, type, handler, options) => {
    target.addEventListener(type, handler, options);
    send.push(() => target.removeEventListener(type, handler, options));
  };
  const pointer = (event) => {
    const [x, y] = canvasPoint(event, canvas.getBoundingClientRect());
    if (event.type === 'pointerdown' && canvas.setPointerCapture) canvas.setPointerCapture(event.pointerId);
    exports.pointer(PHASE[event.type], pointerTypeCode(event.pointerType), event.pointerId,
      event.isPrimary ? 1 : 0, event.type === 'pointermove' ? -1 : event.button, x, y,
      event.pressure || 0, modifierBits(event), event.timeStamp);
    event.preventDefault();
  };
  for (const type of Object.keys(PHASE)) on(canvas, type, pointer);
  on(canvas, 'wheel', (event) => {
    const [x, y] = canvasPoint(event, canvas.getBoundingClientRect());
    exports.wheel(x, y, event.deltaX, event.deltaY, event.deltaMode, modifierBits(event), event.timeStamp);
    event.preventDefault();
  }, { passive: false });
  const key = (down) => (event) => {
    exports.key(down ? 1 : 0, event.key, modifierBits(event), event.repeat ? 1 : 0, event.timeStamp);
    // Keep Space, Enter, Tab and the arrows from scrolling or moving page focus; shortcuts with
    // Control or Meta stay with the browser.
    if (!event.ctrlKey && !event.metaKey) event.preventDefault();
  };
  on(canvas, 'keydown', key(true));
  on(canvas, 'keyup', key(false));
  on(canvas, 'focus', () => exports.focus(1, win.performance.now()));
  on(canvas, 'blur', () => exports.focus(0, win.performance.now()));
  on(doc, 'visibilitychange', () => exports.visibility(doc.visibilityState === 'visible' ? 1 : 0));
  return () => send.forEach((remove) => remove());
}

function resizeCanvas(canvas, win, exports) {
  const ratio = win.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = Math.max(1, Math.round(rect.width * ratio));
  canvas.height = Math.max(1, Math.round(rect.height * ratio));
  exports.resize(rect.width, rect.height, ratio);
}

// Boots one app. `createModule` is the Emscripten MODULARIZE factory of the app's .js. Returns
// {supported: false} after showing the unsupported page, or the live handles.
export async function boot({ createModule, win = globalThis.window, doc = globalThis.document }) {
  if (!webgpuAvailable(win.navigator)) {
    showUnsupported(doc);
    return { supported: false };
  }
  const canvas = doc.getElementById('tessera-canvas');
  canvas.tabIndex = 0;
  const module = { canvas };
  let exports = null;
  const bridge = createFrameBridge({
    raf: (callback) => win.requestAnimationFrame(callback),
    caf: (handle) => win.cancelAnimationFrame(handle),
    setTimer: (callback, ms) => win.setTimeout(callback, ms),
    clearTimer: (handle) => win.clearTimeout(handle),
    onFrame: (timestamp) => exports.frame(timestamp),
  });
  module.tesseraBridge = bridge;
  const instance = await createModule(module);
  const call = (name, types) => (...args) => instance.ccall(name, null, types, args);
  exports = {
    pointer: call('tessera_web_pointer', ['number', 'number', 'number', 'number', 'number', 'number',
      'number', 'number', 'number', 'number']),
    wheel: call('tessera_web_wheel', ['number', 'number', 'number', 'number', 'number', 'number', 'number']),
    key: call('tessera_web_key', ['number', 'string', 'number', 'number', 'number']),
    focus: call('tessera_web_focus', ['number', 'number']),
    resize: call('tessera_web_resize', ['number', 'number', 'number']),
    visibility: call('tessera_web_visibility', ['number']),
    wake: call('tessera_web_wake', []),
    frame: call('tessera_web_frame', ['number']),
  };
  const detach = attachInput({ canvas, exports, win, doc });
  resizeCanvas(canvas, win, exports);
  if (typeof win.ResizeObserver === 'function') {
    new win.ResizeObserver(() => resizeCanvas(canvas, win, exports)).observe(canvas);
  }
  exports.wake();
  return { supported: true, bridge, exports, detach };
}
