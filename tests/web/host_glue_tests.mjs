// SPDX-License-Identifier: MIT
// Issue #290: the host page glue (src/platform/web/host/tessera_web_host.js) under Node, with fake
// DOM objects. No browser: the pure helpers and the on-demand frame bridge are what is checked.

import assert from 'node:assert/strict';
import {
  MOD_ALT, MOD_CAPS, MOD_CONTROL, MOD_META, MOD_SHIFT, attachInput, boot, canvasPoint,
  createFrameBridge, modifierBits, pointerTypeCode, showUnsupported, webgpuAvailable,
} from '../../src/platform/web/host/tessera_web_host.js';

// Modifier bits are the contract with web_input_translation.hpp (kMod*).
assert.deepEqual([MOD_SHIFT, MOD_CONTROL, MOD_ALT, MOD_META], [1, 2, 4, 8]);
assert.equal(modifierBits({ shiftKey: true, altKey: true, getModifierState: (n) => n === 'CapsLock' }),
  MOD_SHIFT | MOD_ALT | MOD_CAPS);
assert.equal(modifierBits({}), 0);

assert.equal(pointerTypeCode('mouse'), 0);
assert.equal(pointerTypeCode('touch'), 1);
assert.equal(pointerTypeCode('pen'), 2);
assert.equal(pointerTypeCode('hologram'), 3);
assert.equal(pointerTypeCode('toString'), 3);

assert.equal(webgpuAvailable({}), false);
assert.equal(webgpuAvailable(undefined), false);
assert.equal(webgpuAvailable({ gpu: { requestAdapter() {} } }), true);
assert.deepEqual(canvasPoint({ clientX: 110, clientY: 70 }, { left: 100, top: 50 }), [10, 20]);

// The frame bridge: one pending rAF at most, one timer at most, nothing when parked.
{
  let rafCalls = 0;
  const callbacks = [];
  let timers = 0;
  let cleared = 0;
  let timerFn = null;
  const frames = [];
  const bridge = createFrameBridge({
    raf: (cb) => { rafCalls += 1; callbacks.push(cb); return rafCalls; },
    caf: () => {},
    setTimer: (cb) => { timers += 1; timerFn = cb; return timers; },
    clearTimer: () => { cleared += 1; },
    onFrame: (t) => frames.push(t),
  });
  assert.equal(rafCalls, 0, 'nothing is requested at construction');
  bridge.requestFrame();
  bridge.requestFrame();
  assert.equal(rafCalls, 1, 'requests coalesce while one is pending');
  assert.equal(bridge.pending(), true);
  callbacks.shift()(16);
  assert.deepEqual(frames, [16]);
  assert.equal(bridge.pending(), false, 'parked after the callback');
  assert.equal(rafCalls, 1, 'no further callback without a request');
  bridge.wakeAfter(250);
  bridge.wakeAfter(100);
  assert.equal(cleared, 1, 'a second timer replaces the first');
  timerFn();
  assert.equal(rafCalls, 2, 'the timer requests exactly one frame');
  bridge.cancel();
  assert.equal(bridge.pending(), false);
}

// Unsupported browser: the page is shown, the canvas hidden, and the Wasm is never created.
{
  const elements = { unsupported: { hidden: true }, 'tessera-canvas': { hidden: false } };
  const doc = { getElementById: (id) => elements[id] };
  showUnsupported(doc);
  assert.equal(elements.unsupported.hidden, false);
  assert.equal(elements['tessera-canvas'].hidden, true);
  let created = false;
  elements.unsupported.hidden = true;
  const result = await boot({
    createModule: async () => { created = true; },
    win: { navigator: {} },
    doc,
  });
  assert.equal(result.supported, false);
  assert.equal(created, false, 'no module without WebGPU');
  assert.equal(elements.unsupported.hidden, false);
}

// Input wiring: pointer, wheel and key events reach the exports with the flattened numbers.
{
  const listeners = new Map();
  const target = () => ({
    addEventListener: (type, fn) => listeners.set(type, fn),
    removeEventListener: (type) => listeners.delete(type),
  });
  const canvas = { ...target(), getBoundingClientRect: () => ({ left: 10, top: 20 }), setPointerCapture() {} };
  const doc = { ...target(), visibilityState: 'hidden' };
  const calls = [];
  const exports = {
    pointer: (...a) => calls.push(['pointer', ...a]),
    wheel: (...a) => calls.push(['wheel', ...a]),
    key: (...a) => calls.push(['key', ...a]),
    focus: (...a) => calls.push(['focus', ...a]),
    visibility: (...a) => calls.push(['visibility', ...a]),
  };
  const detach = attachInput({ canvas, exports, win: { performance: { now: () => 5 } }, doc });
  let prevented = 0;
  const preventDefault = () => { prevented += 1; };
  listeners.get('pointerdown')({
    type: 'pointerdown', pointerId: 3, pointerType: 'touch', isPrimary: true, button: 0, clientX: 15,
    clientY: 30, pressure: 0.5, timeStamp: 99, preventDefault,
  });
  assert.deepEqual(calls.pop(), ['pointer', 0, 1, 3, 1, 0, 5, 10, 0.5, 0, 99]);
  listeners.get('pointermove')({
    type: 'pointermove', pointerId: 3, pointerType: 'mouse', isPrimary: false, button: 0, clientX: 10,
    clientY: 20, timeStamp: 100, preventDefault,
  });
  assert.equal(calls.pop()[5], -1, 'a move reports no button');
  listeners.get('wheel')({ clientX: 10, clientY: 20, deltaX: 0, deltaY: 100, deltaMode: 0, timeStamp: 1, preventDefault });
  assert.deepEqual(calls.pop(), ['wheel', 0, 0, 0, 100, 0, 0, 1]);
  listeners.get('keydown')({ key: 'Enter', repeat: false, timeStamp: 2, preventDefault });
  assert.deepEqual(calls.pop(), ['key', 1, 'Enter', 0, 0, 2]);
  listeners.get('keydown')({ key: 'c', ctrlKey: true, repeat: false, timeStamp: 3, preventDefault });
  assert.equal(prevented, 4, 'ctrl chords stay with the browser (no preventDefault)');
  calls.length = 0;
  listeners.get('visibilitychange')();
  assert.deepEqual(calls.pop(), ['visibility', 0]);
  detach();
  assert.equal(listeners.size, 0, 'detach removes every listener');
}

console.log('web host glue: ok');
