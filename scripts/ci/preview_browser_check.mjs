// SPDX-License-Identifier: MIT
//
// Browser verification for the merged studio page (issue #392), driven through Chrome DevTools
// Protocol with no external dependency.
//
//   node scripts/ci/preview_browser_check.mjs --chrome "C:\Program Files\Google\Chrome\Application\chrome.exe"
//
// **Why this exists, and why it is not in CI.** Issue #392's acceptance is a browser statement: the
// deployed `index.html` shows diagnostics, the scene snapshot and the WebGPU canvas from one page and
// one set of controls. (The old `scene.html`/`canvas.html` redirect stubs were removed.) The static checks in `preview_page_checks.mjs` prove the wiring is
// *consistent*; only a browser proves it *runs*. No CI lane here has one (the Pages lane has no
// browser, and the canvas pixel lane is an owner decision -- docs/design/compiler-wasm.md 8.3), so
// this is a developer-run check, and its output is the evidence recorded for the slice.
//
// It uses the local Chrome through the DevTools Protocol over a WebSocket, with `node:` builtins only
// (`http`, `child_process`, `crypto`) plus Node 22+'s global `WebSocket`. No Playwright, no npm install:
// adding a browser dependency is a spec/ADR decision (`docs/specs/dependencies.md`), and the merge must
// not smuggle one in.
//
// **What it asserts.** Four groups, each a claim in the issue's acceptance:
//
//   1. the merged page loads and Diagnostics runs (the default view, first paint);
//   2. the Scene view opens on its tab, fetches its module *then* (not before), and lists nodes;
//   3. a `click 101` script produces state cells in the Scene view;
//   4. the request log shows no scene or canvas module fetched before those views were opened.
//
// The Canvas view's WebGPU frame is checked as a *state*, not as pixels: a headless GPU may or may not
// be present, and both outcomes are correct as long as the pane says which one it is rather than
// showing an empty canvas. The pixel comparison is P3's separate, owner-gated browser lane.
//
// Issue #375 adds the device handover: when the browser can give the page a WebGPU adapter, the Canvas
// view must draw (draw calls > 0), because a module that received null handles shows the unsupported
// state instead. Without an adapter the step prints SKIPPED rather than passing silently. The
// unsupported state also records which side failed (`data-cause`), and the heading must agree.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '..', '..');
const SITE = resolve(REPO_ROOT, 'build', 'pages');

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.tsx': 'text/plain; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
  '.svg': 'image/svg+xml',
};

let failures = 0;
let passed = 0;

function ok(name) {
  passed += 1;
  process.stdout.write(`ok   ${name}\n`);
}

function fail(name, detail) {
  failures += 1;
  process.stdout.write(`FAIL ${name}\n     ${detail}\n`);
}

async function test(name, body) {
  try {
    await body();
    ok(name);
  } catch (error) {
    fail(name, error && error.message ? error.message : String(error));
  }
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function assertEqual(got, want, message) {
  if (got !== want) {
    throw new Error(`${message}: got ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
  }
}

// --- a no-store static server ---------------------------------------------------------------
//
// `Cache-Control: no-store` is not optional: a browser caches a `.wasm` artifact, and a stale module
// next to a new page is the exact mismatch that makes a merged page look broken for reasons that have
// nothing to do with the merge. The issue's farming notes say to serve the directory this way.

function startServer() {
  const server = createServer(async (request, response) => {
    const url = new URL(request.url, 'http://127.0.0.1');
    let path = decodeURIComponent(url.pathname);
    if (path.endsWith('/')) {
      path += 'index.html';
    }
    const target = resolve(SITE, '.' + path);
    if (!target.startsWith(SITE + sep) && target !== SITE) {
      response.writeHead(403).end('forbidden');
      return;
    }
    try {
      const body = await readFile(target);
      response.writeHead(200, {
        'Content-Type': MIME[extname(target)] || 'application/octet-stream',
        'Cache-Control': 'no-store',
        // The merged page's modules are ES modules and the wasm is fetched by the loader, so the
        // cross-origin isolation headers are deliberately absent: ADR-045 decision 5 keeps the build
        // single-threaded and the page must not depend on `SharedArrayBuffer` being available.
      });
      response.end(body);
    } catch {
      response.writeHead(404, { 'Cache-Control': 'no-store' }).end('not found');
    }
  });
  return new Promise((resolvePromise) => {
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      resolvePromise({ server, base: `http://127.0.0.1:${port}` });
    });
  });
}

// --- a minimal DevTools Protocol client -----------------------------------------------------

class Cdp {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    ws.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve: done, reject } = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) {
          reject(new Error(message.error.message));
        } else {
          done(message.result);
        }
        return;
      }
      this.events.push(message);
    });
  }

  send(method, params = {}) {
    const id = this.nextId;
    this.nextId += 1;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((resolvePromise, reject) => {
      this.pending.set(id, { resolve: resolvePromise, reject });
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`CDP timeout: ${method}`));
        }
      }, 30000);
    });
  }

  /** Waits for an event of `method` whose predicate accepts it, or rejects after `timeout`. */
  waitFor(method, predicate = () => true, timeout = 20000) {
    const started = Date.now();
    return new Promise((resolvePromise, reject) => {
      const poll = () => {
        const index = this.events.findIndex((event) => event.method === method && predicate(event));
        if (index >= 0) {
          const [event] = this.events.splice(index, 1);
          resolvePromise(event.params);
          return;
        }
        if (Date.now() - started > timeout) {
          reject(new Error(`timed out waiting for ${method}`));
          return;
        }
        setTimeout(poll, 25);
      };
      poll();
    });
  }

  /** Evaluates `expression` in the page and returns its JSON value. */
  async evaluate(expression) {
    const result = await this.send('Runtime.evaluate', {
      expression,
      awaitPromise: true,
      returnByValue: true,
    });
    if (result.exceptionDetails) {
      throw new Error(
        `page exception: ${result.exceptionDetails.exception?.description || result.exceptionDetails.text}`,
      );
    }
    return result.result.value;
  }

  close() {
    this.ws.close();
  }
}

async function connect(port) {
  // The browser-level endpoint does not accept `Page.*`; those live on a *page target*, which is what
  // `/json/list` names. The rump page Chrome is started with is the target this drives.
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await response.json();
      const page = targets.find((target) => target.type === 'page' && target.webSocketDebuggerUrl);
      if (page) {
        const ws = new WebSocket(page.webSocketDebuggerUrl);
        await new Promise((res, rej) => {
          ws.addEventListener('open', res, { once: true });
          ws.addEventListener('error', rej, { once: true });
        });
        return new Cdp(ws);
      }
    } catch {
      /* the endpoint is not up yet */
    }
    await new Promise((res) => setTimeout(res, 100));
  }
  throw new Error('Chrome did not expose a page target');
}

// --- the pages under test -------------------------------------------------------------------

const SAMPLE_SOURCE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="101" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

async function main() {
  if (!existsSync(join(SITE, 'index.html'))) {
    process.stderr.write(
      `SETUP ERROR: ${SITE} has no index.html. Assemble the site first (the Pages lane's "Assemble\n` +
        'site" step).\n',
    );
    return 2;
  }

  const args = process.argv.slice(2);
  const chrome = args.find((value, index) => args[index - 1] === '--chrome') ||
    process.env.CHROME_PATH ||
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
  if (!existsSync(chrome)) {
    process.stderr.write(`SETUP ERROR: no Chrome at ${chrome}; pass --chrome PATH.\n`);
    return 2;
  }

  const { server, base } = await startServer();
  const profile = await mkdtemp(join(tmpdir(), 'tessera-392-'));
  const port = 9333;
  const browser = spawn(
    chrome,
    [
      '--headless=new',
      `--remote-debugging-port=${port}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-gpu-sandbox',
      `--user-data-dir=${profile}`,
      'about:blank',
    ],
    { stdio: 'ignore' },
  );

  let cdp = null;
  try {
    cdp = await connect(port);
    await cdp.send('Page.enable');
    await cdp.send('Runtime.enable');
    await cdp.send('Network.enable');

    // Every request the page makes, so the "no scene/canvas module before its view" claim is measured
    // rather than asserted from the source.
    const requests = [];
    cdp.ws.addEventListener('message', (event) => {
      const message = JSON.parse(event.data);
      if (message.method === 'Network.requestWillBeSent') {
        requests.push(message.params.request.url);
      }
    });

    /**
     * Navigates and waits for *that* document, not the one already on screen.
     *
     * The distinction is not pedantry: two of these cases navigate to a URL whose expected end state
     * the previous page already satisfied (it had the Scene tab selected from an earlier click), and a
     * ready-predicate alone then passes instantly against the stale document while the new one is still
     * loading -- the test goes green on the wrong page. Each navigation therefore installs a script that
     * stamps its own token into the *next* document, so "the page I asked for is the page I am reading"
     * is an identity check rather than a guess about timing.
     */
    /**
     * Loads a URL as a **fresh document**, then waits for `waitFor.ready`.
     *
     * Going through `about:blank` first is not superstition. A `Page.navigate` to a URL that differs
     * from the current one only in its fragment is a *same-document* navigation: no new document, no
     * scripts run, and the merged page -- which reads its fragment once at load -- would keep the old
     * state while the address bar showed the new link. That is a real behaviour of the page (a fragment
     * typed into the bar of a loaded page does nothing until a reload), but it is not what these cases
     * are testing, and the fragment cases here differ from the preceding URL exactly that way. A
     * stop at `about:blank` guarantees a new document, so each case measures the arrival path a visitor
     * with a link actually takes.
     */
    async function navigate(url, waitFor) {
      await cdp.send('Page.navigate', { url: 'about:blank' });
      await waitUntil(cdp, `document.readyState === 'complete'`, 'the blank page');
      await cdp.send('Page.navigate', { url });
      // A redirect stub lands somewhere else on purpose, so the final location is the caller's to name
      // when it is not the URL that was requested.
      const expect = waitFor.expect || url;
      await waitUntil(
        cdp,
        `document.readyState === 'complete' && location.href === ${JSON.stringify(expect)}`,
        `the document at ${expect} to load`,
      );
      await waitUntil(cdp, waitFor.ready, waitFor.what);
    }

    // --- 1. the merged page, diagnostics by default ------------------------------------------
    // Since issue #444 the first load applies itself: the pane leaving "ready" for a compiled answer is
    // the automatic first apply, and no button is pressed to make it happen.
    await test('the merged page loads with Diagnostics and compiles the source', async () => {
      await navigate(`${base}/index.html`, {
        ready: `document.getElementById('diag-status') && document.getElementById('diag-status').textContent === 'ready'`,
        what: 'the diagnostics module to load',
      });
      await waitUntil(
        cdp,
        `document.getElementById('diag-status').textContent === 'no diagnostics'`,
        'the automatic first apply to finish',
      );
      const state = await cdp.evaluate(`(() => ({
        title: document.title,
        tabs: [...document.querySelectorAll('[role="tab"]')].map(t => t.textContent.trim()),
        selected: [...document.querySelectorAll('[role="tab"]')].map(t => t.getAttribute('aria-selected')),
        diagStatus: document.getElementById('diag-status').textContent,
        visiblePanes: [...document.querySelectorAll('[role="tabpanel"]')].filter(p => !p.hidden).map(p => p.id),
        controls: [...document.querySelectorAll('#controls select')].length,
      }))()`);
      assertEqual(state.tabs.length, 3, 'three view tabs');
      assertEqual(state.controls, 3, 'one toolbar with viewport, DPR and theme');
      assertEqual(state.diagStatus, 'no diagnostics', 'the compiler reported no diagnostics');
      assert(
        state.selected[0] === 'true' && state.selected[1] === 'false',
        `Diagnostics is the selected tab on arrival: ${state.selected}`,
      );
      assertEqual(state.visiblePanes.join(','), 'view-diagnostics', 'only the diagnostics pane is shown');
    });

    // --- 2. laziness: no scene/canvas module before its view is opened ------------------------
    await test('no scene or canvas module is fetched on the first paint', async () => {
      const before = requests.filter((url) => /tessera_(scene_preview|canvas)\.(js|wasm)/.test(url));
      assertEqual(
        before.length,
        0,
        `the first paint fetched a lazy module: ${JSON.stringify(before)}`,
      );
      const diagnostics = requests.filter((url) => /tessera_diagnostics\.(js|wasm)/.test(url));
      assert(diagnostics.length > 0, 'the diagnostics module is on the first-paint path');
    });

    // --- 3. the scene view: opens, fetches its module then, and lists nodes --------------------
    await test('the Scene view loads its module on first open and lists the snapshot', async () => {
      await cdp.evaluate(`document.getElementById('tab-scene').click()`);
      await waitUntil(
        cdp,
        `document.querySelectorAll('#nodes .node').length > 0`,
        'the scene view to render its nodes',
      );
      const state = await cdp.evaluate(`(() => ({
        nodes: [...document.querySelectorAll('#nodes .node')].map(n => n.textContent),
        status: document.getElementById('scene-status').textContent,
        visible: [...document.querySelectorAll('[role="tabpanel"]')].filter(p => !p.hidden).map(p => p.id),
      }))()`);
      assert(state.nodes.length > 0, 'the snapshot has nodes');
      assert(
        state.nodes.some((text) => /Button/.test(text)),
        `the snapshot includes the Button node: ${JSON.stringify(state.nodes)}`,
      );
      assertEqual(state.visible.join(','), 'view-scene', 'only the scene pane is shown');
      const fetched = requests.filter((url) => /tessera_scene_preview\.js/.test(url));
      assertEqual(fetched.length, 1, 'the scene module was fetched exactly once, when the view opened');
    });

    // --- 4. the click script produces state cells -------------------------------------------
    await test('a click script produces state cells in the Scene view', async () => {
      await cdp.evaluate(`(() => {
        const script = document.getElementById('script');
        script.value = 'click 101\\nclick 101\\nclick 101\\n';
        document.getElementById('run-input').click();
      })()`);
      await waitUntil(
        cdp,
        `document.querySelectorAll('#cells .cell').length >= 2`,
        'the state cells to render',
      );
      const cells = await cdp.evaluate(`(() => ({
        rows: [...document.querySelectorAll('#cells .cell')].map(r => r.textContent),
        status: document.getElementById('input-status').textContent,
      }))()`);
      assert(cells.rows.length >= 2, 'the initial state plus at least one step');
      assert(
        cells.rows.some((row) => /count/.test(row)),
        `the cells name the state cell: ${JSON.stringify(cells.rows)}`,
      );
      assert(
        /handler run/.test(cells.status) || /step/.test(cells.status),
        `the input status reports the run: ${cells.status}`,
      );
    });

    // --- 5. the canvas view: a named state, never a blank canvas -----------------------------
    await test('the Canvas view opens and names its state (unsupported or a frame)', async () => {
      await cdp.evaluate(`document.getElementById('tab-canvas').click()`);
      await waitUntil(
        cdp,
        `(() => {
          const status = document.getElementById('canvas-status');
          const unsupported = document.getElementById('unsupported');
          return status && (status.textContent !== 'not started' || (unsupported && !unsupported.hidden));
        })()`,
        'the canvas view to decide its state',
      );
      await cdp.evaluate(`document.getElementById('restart-app').click()`);
      await waitUntil(
        cdp,
        `(() => {
          const status = document.getElementById('canvas-status').textContent;
          const unsupported = !document.getElementById('unsupported').hidden;
          const frame = !document.getElementById('canvas-frame').hidden;
          return unsupported || frame || /fail|drew|no draw/.test(status);
        })()`,
        'the canvas view to report a frame or the named unsupported state',
      );
      const state = await cdp.evaluate(`(() => ({
        status: document.getElementById('canvas-status').textContent,
        unsupported: !document.getElementById('unsupported').hidden,
        reason: document.getElementById('unsupported-body').textContent,
        heading: document.getElementById('unsupported-heading').textContent,
        cause: document.getElementById('unsupported').dataset.cause || '',
        frame: !document.getElementById('canvas-frame').hidden,
        draws: document.getElementById('stat-draws').textContent,
      }))()`);
      // Both outcomes are correct. What is NOT correct is a pane that reports neither.
      assert(
        state.unsupported || state.frame || /drew|no draw/.test(state.status),
        `the canvas view reported neither a frame nor a named state: ${JSON.stringify(state)}`,
      );
      if (state.unsupported) {
        assert(
          state.reason.length > 20 && /WebGPU|navigator\.gpu|adapter|device/i.test(state.reason),
          `the unsupported state names its reason: ${state.reason}`,
        );
        process.stdout.write(`     (canvas: unsupported here -- ${state.reason.slice(0, 80)}...)\n`);
      } else {
        process.stdout.write(`     (canvas: ${state.status})\n`);
      }
    });

    // --- 7b. the device handover (issue #375) -------------------------------------------------
    // Whether the page's device reaches the module is a browser fact: a JS object coerced to a null
    // handle once left the canvas blank while every static check stayed green. This runs only where the
    // browser can give the page a WebGPU adapter, because without one there is no device to hand over
    // and the Canvas view correctly shows the browser-side unsupported state. When it cannot run it says
    // so, loudly, instead of passing.
    const adapter = await cdp.evaluate(`(async () => {
      if (!navigator.gpu) return 'no navigator.gpu';
      try { return (await navigator.gpu.requestAdapter()) ? 'adapter' : 'no adapter'; }
      catch (error) { return 'requestAdapter failed: ' + error; }
    })()`);
    if (adapter !== 'adapter') {
      process.stdout.write(
        `     SKIPPED: the device handover was not exercised (${adapter}). Run this check in a ` +
          'WebGPU-capable Chrome to cover it.\n',
      );
    } else {
      await test('with a WebGPU adapter the module takes the device and draws (handover)', async () => {
        const handover = await cdp.evaluate(`(() => ({
          unsupported: !document.getElementById('unsupported').hidden,
          cause: document.getElementById('unsupported').dataset.cause || '',
          reason: document.getElementById('unsupported-body').textContent,
          frame: !document.getElementById('canvas-frame').hidden,
          status: document.getElementById('canvas-status').textContent,
          draws: document.getElementById('stat-draws').textContent,
        }))()`);
        assert(
          !handover.unsupported,
          `the browser has an adapter, so the Canvas view must draw, but it shows the ` +
            `${handover.cause || 'unknown'}-side unsupported state: ${handover.reason}`,
        );
        assert(handover.frame, `a frame is shown: ${JSON.stringify(handover)}`);
        const drew = /drew (\d+) draw call/.exec(handover.status);
        assert(
          drew !== null && Number(drew[1]) > 0,
          `the frame has draw calls (the module has a device): ${handover.status}`,
        );
      });
    }

    await test('the unsupported state names which side failed', async () => {
      const state = await cdp.evaluate(`(() => ({
        unsupported: !document.getElementById('unsupported').hidden,
        heading: document.getElementById('unsupported-heading').textContent,
        cause: document.getElementById('unsupported').dataset.cause || '',
      }))()`);
      if (!state.unsupported) {
        process.stdout.write('     (canvas drew, so there is no unsupported state to read)\n');
        return;
      }
      assert(
        state.cause === 'browser' || state.cause === 'module',
        `the unsupported state records its cause: ${JSON.stringify(state.cause)}`,
      );
      // The heading follows the cause, so a module-side failure never blames the browser.
      assert(
        state.cause === 'module'
          ? !/This browser cannot/.test(state.heading)
          : /This browser cannot/.test(state.heading),
        `the heading matches the ${state.cause} cause: ${state.heading}`,
      );
    });

    // --- 8. no request left the origin ------------------------------------------------------
    await test('no request left the page origin', async () => {
      const foreign = requests.filter((url) => !url.startsWith(base) && !url.startsWith('data:'));
      assertEqual(foreign.length, 0, `external requests: ${JSON.stringify(foreign.slice(0, 5))}`);
    });
  } finally {
    if (cdp) {
      cdp.close();
    }
    browser.kill();
    server.close();
    await rm(profile, { recursive: true, force: true }).catch(() => {});
  }

  process.stdout.write(
    `\nbrowser check: ${passed} passed, ${failures} failed\n`,
  );
  return failures > 0 ? 1 : 0;
}

/** Polls a page expression until it is truthy, or throws with `what` after the timeout. */
async function waitUntil(cdp, expression, what, timeout = 30000) {
  const started = Date.now();
  for (;;) {
    let value = false;
    try {
      value = await cdp.evaluate(`Boolean(${expression})`);
    } catch {
      value = false;
    }
    if (value) {
      return;
    }
    if (Date.now() - started > timeout) {
      throw new Error(`timed out waiting for ${what}`);
    }
    await new Promise((res) => setTimeout(res, 50));
  }
}

main().then(
  (code) => {
    process.exitCode = code;
  },
  (error) => {
    process.stderr.write(`browser check failed: ${error && error.stack ? error.stack : error}\n`);
    process.exitCode = 1;
  },
);
