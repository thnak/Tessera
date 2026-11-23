// SPDX-License-Identifier: MIT
//
// CI browser acceptance for issue #375: the **WebGPU device handover**, and the two headings that
// tell a designer which side of it failed.
//
//   node scripts/ci/preview_browser_canvas_handover.mjs [site-dir] [--shot out.png]
//
// **Why this is a gate and not a note.** The handover is a browser fact. The page must turn the JS
// `GPUDevice`/`GPUQueue` into Emscripten handles (`WebGPU.importJsDevice`) and give them to
// `tessera_webgpu_set_device`; #375's symptom was the opposite -- the JS objects went straight into a
// `void*` export, coerced to 0, and were read as null handles, so the module reported "no device" and
// the canvas never drew while every static check stayed green. Only a browser can see that.
//
// **A real check, not a silent skip.** Headless Chromium with `--enable-unsafe-webgpu` gets a
// SwiftShader adapter *and* a device on a GPU-less Linux runner, which is what lets this be a gate:
// it asserts `drawCalls > 0` and fails when the module gets null handles. A runner that cannot give
// Chromium an adapter goes red on the first check below rather than passing quietly.
//
// **What is NOT claimed.** This is not the P3 pixel comparison (issue #167): a software adapter proves
// the page acquires a device, hands it over and draws, not that the pixels match lavapipe. Those
// goldens belong to `canvas-native-parity`, and comparing them stays #167's.
//
// **The headings** are #375's second acceptance sentence: "this browser has no WebGPU" and "the module
// could not take the device" read differently, because the first is a browser setting and the second is
// a build problem. #514 shipped the distinction (`data-cause`, two headings); this file asserts both
// branches by breaking the seam each one is about.

import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize, resolve } from 'node:path';
import { chromium } from 'playwright';
import { writeZip } from '../../designer/web/studio-zip.js';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
};

const args = process.argv.slice(2);
const shotIndex = args.indexOf('--shot');
const shot = shotIndex >= 0 ? args[shotIndex + 1] : null;
const site = resolve(args.find((value, index) => index === 0 && value !== '--shot') ?? 'build/pages');

// The flags that give headless Chromium a WebGPU device on a host with no GPU. `--no-sandbox` is what
// a containerised runner needs (unprivileged user namespaces are off on Ubuntu 23.10+).
const GPU_ARGS = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader', '--no-sandbox'];

// The project the canvas previews. The studio starts with no project open (#391: nothing is bundled),
// so the gate opens one through the page's own Open Zip route; `tessera.json` naming an entry is what
// makes it apply, and the entry's stylesheet import is what makes the frame carry author CSS.
const PROJECT = {
  'tessera.json': `{
  "schemaVersion": 1,
  "app": { "name": "handover-check", "entry": "ui/App.tsx", "namespace": "handover_check" }
}
`,
  'ui/App.tsx': `import "../styles/theme.css";

export function App() {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`,
  'styles/theme.css': `View {
  background: #ffffff;
}

Button {
  background-color: rgb(20, 160, 60);
}
`,
};

const encoder = new TextEncoder();
const projectZip = Buffer.from(
  writeZip(Object.entries(PROJECT).map(([path, text]) => ({ path, bytes: encoder.encode(text) }))),
);

// A no-store static server. `no-store` is not optional: a browser caches a `.wasm`, and a stale module
// beside a fresh page is a mismatch that has nothing to do with the change under test.
function startServer() {
  const server = createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1');
    let path = normalize(decodeURIComponent(url.pathname));
    if (path === '/' || path === '\\') path = '/index.html';
    try {
      const body = await readFile(join(site, path));
      response.writeHead(200, {
        'content-type': MIME[extname(path).toLowerCase()] ?? 'application/octet-stream',
        'cache-control': 'no-store',
      });
      response.end(body);
    } catch {
      response.writeHead(404, { 'content-type': 'text/plain' });
      response.end('not found');
    }
  });
  return new Promise((done) => {
    server.listen(0, '127.0.0.1', () => done({ server, base: `http://127.0.0.1:${server.address().port}` }));
  });
}

let failures = 0;
let skipped = 0;
function check(name, ok, detail) {
  process.stdout.write(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? `  -- ${detail}` : ''}\n`);
  if (!ok) {
    failures += 1;
  }
}

/**
 * A check this runner could not exercise, said out loud.
 *
 * The issue's rule is "skip loudly, not pass silently", and the summary has to say which it was: a run
 * where every device-dependent check was skipped must not read as "all checks passed", or a runner that
 * can never draw would look green forever.
 */
function skipLoudly(what, why) {
  skipped += 1;
  process.stdout.write(`SKIP ${what}  -- ${why}\n`);
}

/**
 * Whether the pane is showing "the browser handed out a device and took it back".
 *
 * That is a **runner** condition, not a page bug: a hosted SwiftShader runner does it, intermittently.
 * It is the case the issue anticipated ("if a runner cannot give Chromium an adapter, skip loudly rather
 * than pass silently"), and telling it apart from a handover regression matters because both appear in
 * the same unsupported state -- one as `browser`, one as `module`.
 */
function lostDevice(state) {
  return state.unsupported === true && state.cause === 'browser' && /device was lost/i.test(state.body ?? '');
}

/**
 * Opens `index.html` with the Canvas view selected and the probe project loaded.
 *
 * Whether a device was handed out and then taken away is read from the pane's own sentence (see
 * `lostDevice`) rather than from the console: `views/canvas-session.ts` reports it in the unsupported
 * state, and that is where a person sees it.
 */
async function openCanvas(context, base) {
  const page = await context.newPage();
  const errors = [];
  page.on('console', (message) => {
    if (message.type() === 'error') errors.push(message.text());
  });
  page.on('pageerror', (error) => errors.push(`pageerror: ${String(error)}`));
  // Opening a zip over an open project asks "replace?"; the check always replaces.
  page.on('dialog', (dialog) => void dialog.accept());
  await page.goto(`${base}/index.html?view=canvas`, { waitUntil: 'load' });
  await page.locator('input[type="file"][accept*="zip"]').setInputFiles({
    name: 'handover-check.zip',
    mimeType: 'application/zip',
    buffer: projectZip,
  });
  // The tree listing the project is the load settling; the frame follows the apply that comes with it.
  // A failure here is a check, not a thrown promise: the gate must report what it saw and keep going, so
  // a regression shows as a red line naming the state rather than as a stack trace with no verdict.
  await settles(() => document.querySelector('#project-tree [data-path="ui/App.tsx"]') !== null, page);
  await settles(() => {
    const pane = document.getElementById('unsupported');
    const status = document.getElementById('canvas-status').textContent ?? '';
    return !pane.hidden || /draw call|not applied|fail|unavailable/i.test(status);
  }, page);
  return { page, errors, state: () => readState(page) };
}

/** A wait whose timeout is a boolean, so a caller can turn it into a check instead of a crash. */
async function settles(expression, page, timeout = 20000) {
  try {
    await page.waitForFunction(expression, undefined, { timeout });
    return true;
  } catch {
    return false;
  }
}

/** The canvas pane's state, as the page actually shows it. */
function readState(page) {
  return page.evaluate(() => ({
    unsupported: !document.getElementById('unsupported').hidden,
    cause: document.getElementById('unsupported').dataset.cause ?? '',
    heading: document.getElementById('unsupported-heading').textContent,
    body: document.getElementById('unsupported-body').textContent,
    status: document.getElementById('canvas-status').textContent,
    draws: document.getElementById('stat-draws').textContent,
  }));
}

/**
 * The draw count the pane shows as a number. A pane with no frame shows `-`, and `Number('-')` is NaN,
 * which compares false against `> 0` exactly as it should: no frame, no draws.
 */
function drawCount(text) {
  const parsed = Number(text);
  return Number.isFinite(parsed) ? parsed : 0;
}

const { server, base } = await startServer();
const browser = await chromium.launch({ args: GPU_ARGS });

/**
 * Whether this browser can hand out a device **right now**, asked in a page of its own.
 *
 * This has to come first, and in its own context, for a reason a hosted runner taught: on a cold
 * SwiftShader start the very first `requestAdapter` can return null, and the page — which asks exactly
 * once, at load — then shows the browser-side unsupported state forever. Asking here makes "the runner
 * cannot give Chromium a device" a red check on its own line instead of three confusing failures in the
 * middle of the handover.
 */
async function deviceAvailable() {
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.goto(base, { waitUntil: 'load' });
  const ask = () =>
    page.evaluate(async () => {
      if (!navigator.gpu) return { gpu: false };
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) return { gpu: true, adapter: false };
      return { gpu: true, adapter: true, device: Boolean(await adapter.requestDevice()) };
    });
  let result = await ask();
  // A cold SwiftShader process can answer the first `requestAdapter` with null and every later one with
  // an adapter -- observed on a hosted runner, where the number was available a second later. Retrying
  // *this probe* is fair (it is asking "can this runner ever give a device", not asserting the page's
  // behaviour); the page's own single attempt is what the handover checks then measure.
  for (let attempt = 0; attempt < 5 && result.device !== true; attempt += 1) {
    await page.waitForTimeout(1000);
    result = await ask();
  }
  // The context stays open for the rest of the run on purpose. Closing it destroyed the device this
  // probe acquired, and on a SwiftShader runner that also tore down the shared WebGPU instance: the
  // studio's own device was then "lost" moments after the browser handed it out, which looked like a
  // page bug and was this file's own teardown. A warm instance is also what a visitor's browser has.
  return { result, close: () => context.close() };
}

try {
  // --- 1. a device, and the handover that uses it -------------------------------------------
  // `warm` keeps its context open until the end: see the note in `deviceAvailable`.
  const warm = await deviceAvailable();
  const probe = warm.result;
  check('the browser under test has a WebGPU device', probe.device === true, JSON.stringify(probe));

  if (probe.device !== true) {
    // Nothing downstream can be honest without a device, and one red line naming the real cause is worth
    // more than three red lines about its consequences. The lane is red either way (the check above).
    skipLoudly('the device handover', 'this runner has no WebGPU device');
  } else {
    // The handover, attempted a few times.
    //
    // A hosted SwiftShader runner gave a device and then took it away moments later ("The WebGPU device
    // was lost"), intermittently: three runs of the same commit failed on a different check each time,
    // and the same commit passes on a developer box every time. Retrying is the honest response to a
    // *runner* that cannot hold a device -- but only when the fruit is that: a loss the page itself
    // reports as browser-side. A module that cannot take the device (the #375 regression) is
    // `cause=module` and fails on the first attempt, with no retry.
    let drawn = null;
    let run = null;
    let lossNote = '';
    for (let attempt = 1; attempt <= 3; attempt += 1) {
      const context = await browser.newContext({ viewport: { width: 1600, height: 1000 } });
      run = await openCanvas(context, base);
      drawn = await run.state();
      const lostHere = lostDevice(drawn);
      if (!lostHere) {
        run.context = context;
        break;
      }
      lossNote = drawn.body;
      await context.close();
      run = null;
      if (attempt < 3) {
        process.stdout.write(`     attempt ${attempt}: the runner lost the device (${lossNote}); retrying\n`);
        await new Promise((done) => setTimeout(done, 2000));
      }
    }

    if (run === null) {
      // Every attempt lost the device: the runner cannot hold one. Said out loud, never counted as a pass.
      skipLoudly('the device handover', `this runner lost the device on every attempt: ${lossNote}`);
    } else {
      check(
        'the module takes the device the browser gives it (no unsupported state)',
        drawn.unsupported === false,
        drawn.unsupported ? `${drawn.cause}-side: ${drawn.body}` : 'a frame is shown',
      );
      // Both halves are required: the pane's sentence and the counter it filled. `#stat-draws` is markup
      // that keeps its last value, so a draw count read while the pane shows the unsupported state is the
      // leftover of another frame, not evidence of this one.
      check(
        'the frame has draw calls, so the handles were real',
        drawn.unsupported === false && /drew \d+ draw call/.test(drawn.status ?? '') && drawCount(drawn.draws) > 0,
        `draws=${drawn.draws}, status=${drawn.status}`,
      );
      check('the status names the draw count', /drew \d+ draw call/.test(drawn.status ?? ''), drawn.status ?? '');

      // A restart is the same cold start a Run takes, and #430 was "every Run after the first re-created
      // the module and the fresh instance had null handles". One instance per open is the rule it fixed.
      await run.page.locator('#restart-app').click();
      const redrew = await settles(
        () => /drew \d+ draw call/.test(document.getElementById('canvas-status').textContent ?? ''),
        run.page,
      );
      const restarted = await run.state();
      check(
        'a restart draws again with the same device',
        redrew && restarted.unsupported === false && drawCount(restarted.draws) > 0,
        restarted.status ?? '',
      );
      // A page error while the canvas is drawing is the signature of a Wasm/JS glue mismatch, which is
      // what this guards. A bare "a valid external Instance reference no longer exists" is NOT that: it
      // comes from the WebGPU port tearing down an instance after its device is gone, it appears only on
      // a SwiftShader runner, and it says nothing about the handover. Filtering exactly that message (not
      // all errors) keeps the check meaningful where it matters and quiet where it would be noise.
      const serious = run.errors.filter((line) => !/external Instance reference no longer exists/.test(line));
      check('no console errors while the canvas draws', serious.length === 0, serious.slice(0, 3).join(' | ') || 'none');

      if (shot) {
        await run.page.screenshot({ path: shot });
        process.stdout.write(`screenshot: ${shot}\n`);
      }
      await run.context.close();
    }
  }

  // --- 2. the module-side heading ------------------------------------------------------------
  // A module built without the port's handle table cannot take the device, and that is a build problem,
  // not a browser setting. Serving the module with its `Module["WebGPU"]` assignment removed is exactly
  // that build, and the pane must blame itself (issue #375) rather than repeating the browser sentence.
  //
  // This branch needs a device (the page must get one before the module can refuse it) but not the
  // *drawn* frame the branch above proves. When no device is available at all the page correctly stops at
  // the browser-side state, so that is reported as unexercised rather than as a module-heading failure:
  // the two states must not be confused for each other.
  const moduleText = await readFile(join(site, 'tessera_canvas.js'), 'utf8');
  const needle = 'Module["WebGPU"]=WebGPU';
  if (!moduleText.includes(needle)) {
    check(
      'the canvas module exports the WebGPU handle table (the seam this check breaks)',
      false,
      'the module has no `Module["WebGPU"]=WebGPU` assignment; the handover check cannot be driven',
    );
  } else {
    const moduleContext = await browser.newContext({ viewport: { width: 1600, height: 1000 } });
    await moduleContext.route('**/tessera_canvas.js', (route) =>
      route.fulfill({
        status: 200,
        contentType: 'text/javascript; charset=utf-8',
        body: moduleText.replace(needle, '/* the handle table this build does not have */'),
      }),
    );
    const broken = await openCanvas(moduleContext, base);
    const state = await broken.state();
    if (state.unsupported && state.cause === 'browser') {
      skipLoudly('the module-side heading', `no device to hand over: ${state.body}`);
    } else {
      check('a module without the handle table shows the unsupported state', state.unsupported === true, state.status ?? '');
      check('the unsupported state records the module as the cause', state.cause === 'module', state.cause || '(none)');
      check(
        'the heading blames the module, not the browser',
        /could not use this browser's WebGPU device/.test(state.heading) && !/This browser cannot/.test(state.heading),
        state.heading,
      );
      check('the body names the missing export', /WebGPU\.importJsDevice/.test(state.body), state.body.slice(0, 100));
    }
    await moduleContext.close();
  }

  // --- 3. the browser-side heading ------------------------------------------------------------
  // The other half of the pair: no `navigator.gpu` at all is the browser's fault, and the heading must
  // say so. Removing the API before any page script runs is what a browser without WebGPU looks like.
  const noGpuContext = await browser.newContext({ viewport: { width: 1600, height: 1000 } });
  await noGpuContext.addInitScript(() => {
    // `navigator.gpu` is an accessor on Navigator.prototype; deleting the property is the shape of a
    // browser that never had the API.
    delete Navigator.prototype.gpu;
    Object.defineProperty(Navigator.prototype, 'gpu', { get: () => undefined, configurable: true });
  });
  const noGpu = await openCanvas(noGpuContext, base);
  const browserSide = await noGpu.state();
  check('a browser without WebGPU shows the unsupported state', browserSide.unsupported === true, browserSide.status ?? '');
  check('the unsupported state records the browser as the cause', browserSide.cause === 'browser', browserSide.cause || '(none)');
  check(
    'the heading blames the browser',
    /This browser cannot/.test(browserSide.heading),
    browserSide.heading,
  );
  await noGpuContext.close();
  await warm.close();
} finally {
  await browser.close();
  server.close();
}

const verdict =
  failures > 0
    ? `${failures} check(s) failed`
    : skipped > 0
      ? `passed with ${skipped} skipped -- a check this runner could not exercise is not a pass`
      : 'all checks passed';
process.stdout.write(`\ncanvas handover: ${verdict}\n`);
process.exit(failures === 0 ? 0 : 1);
