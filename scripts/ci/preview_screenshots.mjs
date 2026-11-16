// SPDX-License-Identifier: MIT
//
// Screenshots of the merged studio page's three views (issue #392), for the PR's visual evidence.
//
//   node scripts/ci/preview_screenshots.mjs --chrome PATH [--out DIR]
//
// A browser cannot be part of a CI gate here (no lane has one; docs/design/compiler-wasm.md 8.3), so
// the merge's "one page, three views, one set of controls" claim is evidenced by pictures a reviewer
// can look at. This is that evidence generator: it serves `build/pages` with `Cache-Control: no-store`,
// drives the real page, and writes one PNG per view. It reuses the same DevTools Protocol client shape
// as `preview_browser_check.mjs` and adds no dependency.

import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile, mkdtemp, rm } from 'node:fs/promises';
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
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.tsx': 'text/plain; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
};

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
      });
      response.end(body);
    } catch {
      response.writeHead(404).end('not found');
    }
  });
  return new Promise((done) => {
    server.listen(0, '127.0.0.1', () => done({ server, base: `http://127.0.0.1:${server.address().port}` }));
  });
}

class Cdp {
  constructor(ws) {
    this.ws = ws;
    this.nextId = 1;
    this.pending = new Map();
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
      }
    });
  }

  send(method, params = {}) {
    const id = this.nextId;
    this.nextId += 1;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((done, reject) => {
      this.pending.set(id, { resolve: done, reject });
      setTimeout(() => {
        if (this.pending.has(id)) {
          this.pending.delete(id);
          reject(new Error(`CDP timeout: ${method}`));
        }
      }, 30000);
    });
  }

  async evaluate(expression) {
    const result = await this.send('Runtime.evaluate', {
      expression,
      awaitPromise: true,
      returnByValue: true,
    });
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.text);
    }
    return result.result.value;
  }

  close() {
    this.ws.close();
  }
}

async function connect(port) {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = targets.find((target) => target.type === 'page' && target.webSocketDebuggerUrl);
      if (page) {
        const ws = new WebSocket(page.webSocketDebuggerUrl);
        await new Promise((done, reject) => {
          ws.addEventListener('open', done, { once: true });
          ws.addEventListener('error', reject, { once: true });
        });
        return new Cdp(ws);
      }
    } catch {
      /* not up yet */
    }
    await new Promise((done) => setTimeout(done, 100));
  }
  throw new Error('Chrome did not expose a page target');
}

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
    await new Promise((done) => setTimeout(done, 50));
  }
}

async function main() {
  if (!existsSync(join(SITE, 'index.html'))) {
    process.stderr.write(`SETUP ERROR: ${SITE} has no index.html; assemble the site first.\n`);
    return 2;
  }
  const argv = process.argv.slice(2);
  const chrome = argv[argv.indexOf('--chrome') + 1] && argv.includes('--chrome')
    ? argv[argv.indexOf('--chrome') + 1]
    : 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
  if (!existsSync(chrome)) {
    process.stderr.write(`SETUP ERROR: no Chrome at ${chrome}; pass --chrome PATH.\n`);
    return 2;
  }
  const out = argv.includes('--out') ? argv[argv.indexOf('--out') + 1] : resolve(REPO_ROOT, 'build', 'screenshots');
  await mkdir(out, { recursive: true });

  const { server, base } = await startServer();
  const profile = await mkdtemp(join(tmpdir(), 'tessera-shots-'));
  const port = 9334;
  const browser = spawn(
    chrome,
    [
      '--headless=new',
      `--remote-debugging-port=${port}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--window-size=1280,900',
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
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width: 1280,
      height: 900,
      deviceScaleFactor: 1,
      mobile: false,
    });

    async function load(url, ready, what) {
      await cdp.send('Page.navigate', { url: 'about:blank' });
      await waitUntil(cdp, `document.readyState === 'complete'`, 'the blank page');
      await cdp.send('Page.navigate', { url });
      await waitUntil(
        cdp,
        `document.readyState === 'complete' && location.href === ${JSON.stringify(url)}`,
        `the document at ${url}`,
      );
      await waitUntil(cdp, ready, what);
    }

    async function shoot(name) {
      const shot = await cdp.send('Page.captureScreenshot', { format: 'png' });
      const file = join(out, `${name}.png`);
      await writeFile(file, Buffer.from(shot.data, 'base64'));
      process.stdout.write(`wrote ${file}\n`);
    }

    // Diagnostics: the default view, after a Run.
    await load(
      `${base}/index.html`,
      `document.getElementById('diag-status')?.textContent === 'ready'`,
      'the diagnostics module',
    );
    await cdp.evaluate(`document.getElementById('run').click()`);
    await waitUntil(cdp, `document.getElementById('diag-status').textContent === 'no diagnostics'`, 'the run');
    await shoot('round-392-diagnostics');

    // Scene: its tab, its own sample (the one with an automationId), and three clicks.
    await cdp.evaluate(`document.getElementById('tab-scene').click()`);
    await waitUntil(cdp, `document.querySelectorAll('#nodes .node').length > 0`, 'the scene nodes');
    await cdp.evaluate(`document.getElementById('load-sample').click()`);
    await waitUntil(cdp, `document.querySelectorAll('#nodes .node').length > 0`, 'the scene sample');
    await cdp.evaluate(`document.getElementById('run-input').click()`);
    await waitUntil(cdp, `document.querySelectorAll('#cells .cell').length >= 2`, 'the state cells');
    await shoot('round-392-scene');

    // Canvas: its tab, then a Run. The tab's first open imports the module, so the Run is pressed only
    // once the shared Run button is enabled again -- pressing it earlier is a click on a disabled
    // button, which does nothing and then looks like the canvas never started.
    await cdp.evaluate(`document.getElementById('tab-canvas').click()`);
    await waitUntil(
      cdp,
      `!document.getElementById('run').disabled ||
        !document.getElementById('unsupported').hidden`,
      'the canvas module to load (or the browser to refuse WebGPU)',
    );
    await cdp.evaluate(`document.getElementById('run').click()`);
    await waitUntil(
      cdp,
      `!document.getElementById('canvas-frame').hidden || !document.getElementById('unsupported').hidden ||
        /fail|drew|no draw/.test(document.getElementById('canvas-status').textContent)`,
      'the canvas state',
    );
    await shoot('round-392-canvas');

    // Phone width, to evidence the 16 px gutter and no horizontal scroll.
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width: 390,
      height: 844,
      deviceScaleFactor: 2,
      mobile: true,
    });
    await load(
      `${base}/index.html?view=scene`,
      `document.querySelectorAll('#nodes .node').length > 0`,
      'the scene view at phone width',
    );
    const overflow = await cdp.evaluate(
      `({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth })`,
    );
    process.stdout.write(
      `phone width: scrollWidth=${overflow.scroll} clientWidth=${overflow.client} ` +
        `(no horizontal scroll: ${overflow.scroll <= overflow.client})\n`,
    );
    await shoot('round-392-phone');
  } finally {
    if (cdp) {
      cdp.close();
    }
    browser.kill();
    server.close();
    await rm(profile, { recursive: true, force: true }).catch(() => {});
  }
  return 0;
}

main().then(
  (code) => {
    process.exitCode = code;
  },
  (error) => {
    process.stderr.write(`screenshots failed: ${error && error.stack ? error.stack : error}\n`);
    process.exitCode = 1;
  },
);
