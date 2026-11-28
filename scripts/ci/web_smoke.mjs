// SPDX-License-Identifier: MIT
// Issue #969: runs the built web_smoke app under Node (the module is linked for web and node) and checks
// the web-app contract without a browser: it boots, a wake produces exactly one frame request, a frame
// callback runs the C++ frame step once, an input event is consumed, and a parked app holds no pending
// frame. Then it checks the download budgets in tools/web_budgets.json.
//
// Usage: node web_smoke.mjs <app dir> <budgets.json> [--app web_smoke]

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [dir, budgetsPath] = process.argv.slice(2);
const appName = process.argv.includes('--app') ? process.argv[process.argv.indexOf('--app') + 1] : 'web_smoke';
if (!dir || !budgetsPath) {
  console.error('usage: web_smoke.mjs <app dir> <budgets.json> [--app name]');
  process.exit(2);
}

const hostUrl = pathToFileURL(resolve(dir, 'tessera_web_host.js')).href;
const { createFrameBridge } = await import(hostUrl);

let rafCallbacks = 0;
let pending = null;
const bridge = createFrameBridge({
  raf: (cb) => { rafCallbacks += 1; pending = cb; return rafCallbacks; },
  caf: () => { pending = null; },
  setTimer: () => 1,
  clearTimer: () => {},
  onFrame: () => {},
});

const started = performance.now();
const { default: createModule } = await import(pathToFileURL(resolve(dir, 'app.js')).href);
const instance = await createModule({ canvas: {}, tesseraBridge: bridge });
const call = (name, types, args) => instance.ccall(name, 'number', types, args);

assert.equal(rafCallbacks, 0, 'booting requests no frame by itself');
call('tessera_web_wake', [], []);
assert.equal(rafCallbacks, 1, 'a wake requests exactly one frame');
call('tessera_web_wake', [], []);
assert.equal(rafCallbacks, 1, 'a second wake while one is pending requests nothing');
call('tessera_web_pointer', ['number', 'number', 'number', 'number', 'number', 'number', 'number',
  'number', 'number', 'number'], [0, 0, 1, 1, 0, 4, 5, 0.5, 0, 1]);
assert.equal(rafCallbacks, 1, 'input while a frame is pending shares it');

const frame = (ts) => { pending = null; call('tessera_web_frame', ['number'], [ts]); };
frame(16);
const firstFrameMs = performance.now() - started;
assert.equal(call('tessera_smoke_frames', [], []), 1, 'the frame step ran once');
assert.equal(call('tessera_smoke_events', [], []), 1, 'the queued pointer event was consumed');
const afterFrame = rafCallbacks;
assert.equal(pending, null, 'an idle app holds no pending frame callback');
assert.equal(rafCallbacks, afterFrame, 'no further callbacks without a wake');

const budgets = JSON.parse(readFileSync(budgetsPath, 'utf8'))[appName];
assert.ok(budgets, 'no budget entry for ' + appName);
const size = (file) => gzipSync(readFileSync(join(dir, file)), { level: 9 }).length;
const measured = { wasm_gzip: size('app.wasm'), js_gzip: size('app.js'), host_gzip: size('tessera_web_host.js') };
let failed = false;
for (const [key, value] of Object.entries(measured)) {
  const ok = value <= budgets[key];
  failed ||= !ok;
  console.log((ok ? 'ok   ' : 'OVER ') + key + ' ' + value + ' / ' + budgets[key]);
}
const frameOk = firstFrameMs <= budgets.first_frame_ms;
failed ||= !frameOk;
console.log((frameOk ? 'ok   ' : 'OVER ') + 'first_frame_ms ' + Math.round(firstFrameMs) + ' / ' + budgets.first_frame_ms);
process.exit(failed ? 1 : 0);
