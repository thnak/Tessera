// SPDX-License-Identifier: MIT
//
// Browser acceptance for #976: the **Resources** section of the Platform events panel, driven through the
// real page (pending-ui-v1.md section 10.4, designer-protocol-v1.md section 5.2).
//
//   node scripts/ci/preview_browser_resources.mjs [site-dir]
//
// `preview_resource_controls.mjs` proves the vocabulary and the engine cycle under Node, where no WebGPU
// device exists. This gate is the DOM half: a project whose `ui/services.json` declares a service and whose
// entry reads `useResource(..., { suspend: true })` opens on the fallback, the panel lists the resource
// from the engine's report, and Resolve, a stale-epoch Resolve, Fetch and Fail change the status, the
// epoch and the summary sentence exactly as the engine says. The page keeps no copy of the state.
//
// Not a repository test: run it from a directory whose node_modules has playwright (like its neighbours).
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { editEntry, ensureProject, openPanel, tab } from './studio_project.mjs';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.ttf': 'font/ttf',
  '.txt': 'text/plain; charset=utf-8',
};
const site = process.argv[2] ?? 'build/pages';
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
    response.writeHead(404);
    response.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'],
});
const page = await (await browser.newContext()).newPage();
const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));
await page.goto(base, { waitUntil: 'load' });

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}
const settle = (ms = 500) => page.waitForTimeout(ms);

const SERVICES = JSON.stringify({
  version: 1,
  services: [
    {
      id: 'demo.nav',
      name: 'Navigator',
      cpp: { type: 'demo::Navigator', header: 'demo/services.hpp' },
      methods: [{ name: 'loadFeed', params: [{ name: 'mode', type: 'string' }], returns: 'string' }],
    },
  ],
});
const SOURCE = [
  'import { useState, useService, useResource, useTransition, Suspense } from "tessera";',
  'export function Feed() {',
  '  const nav = useService<Navigator>();',
  '  const [mode, setMode] = useState<"fast" | "slow">("fast");',
  '  const [isPending, startTransition] = useTransition();',
  '  const feed = useResource(nav.loadFeed, { mode }, { suspend: true });',
  '  return (',
  '    <View>',
  '      <Suspense fallback={<Text>Loading</Text>}>',
  '        <View><Text>{feed.data}</Text></View>',
  '      </Suspense>',
  '      <Button onClick={() => startTransition(() => setMode("slow"))}><Text>Slow</Text></Button>',
  '      {isPending && <Text>Updating</Text>}',
  '    </View>',
  '  );',
  '}',
  '',
].join('\n');

check('a project with ui/services.json opens', await ensureProject(page, { extraFiles: [['ui/services.json', SERVICES]], force: true }));
await tab(page, 'canvas');
await settle(600);
await editEntry(page, SOURCE);
await page.locator('#restart-app').click();
await settle(2500);

check('the Platform events panel opens', await openPanel(page, 'platform-events'));
await page.locator('#flexlayout-tabbutton-platform-events').click();
await settle(400);

const panel = page.locator('#resource-panel');
await panel.waitFor({ state: 'attached', timeout: 8000 }).catch(() => {});
check('the Resources section appears for a component with useResource', (await panel.count()) === 1);
const row = page.locator('.resource-row[data-resource="res::feed"]');
const rowText = async () => ((await row.locator('.resource-status').textContent()) ?? '').trim();
const summary = async () => ((await page.locator('#resource-summary').textContent()) ?? '').trim();
const notice = async () => ((await page.locator('#resource-notice').textContent()) ?? '').trim();
const press = async (action) => {
  await row.locator(`button[data-action="${action}"]`).click();
  await settle();
};

check('it opens on a fetch in flight (epoch 1) and the boundary shows its fallback', /^loading \(epoch 1\)/.test(await rowText()) && /fallback/.test(await summary()), `${await rowText()} | ${await summary()}`);

await row.locator('.resource-value').fill('hello');
await press('resolve');
check('Resolve restores the content: status ready, no fallback', /^ready \(epoch 1\)/.test(await rowText()) && /content is showing/.test(await summary()), `${await rowText()} | ${await summary()}`);

await row.locator('.resource-epoch').fill('7');
await press('resolve');
check('a Resolve for a stale epoch is dropped and the panel says so', /dropped/.test(await notice()) && /^ready \(epoch 1\)/.test(await rowText()), `${await notice()} | ${await rowText()}`);
await row.locator('.resource-epoch').fill('');

await press('fetch');
check('Fetch starts the next epoch and the content is held with isPending', /\(epoch 2\)/.test(await rowText()) && /isPending is true/.test(await summary()), `${await rowText()} | ${await summary()}`);

await row.locator('.resource-value').fill('boom');
await press('fail');
check('Fail reports the error and keeps the data on screen', /boom/.test(await rowText()) && /content is showing/.test(await summary()), `${await rowText()} | ${await summary()}`);

await row.locator('.resource-epoch').fill('abc');
await press('resolve');
check('a malformed epoch is refused before anything is sent', /whole number/.test(await notice()), await notice());

check('No console errors', consoleErrors.filter((entry) => !/Failed to load resource/.test(entry)).length === 0, consoleErrors.slice(0, 3).join(' | ') || 'none');
console.log(`\n${failures === 0 ? 'all resource panel checks passed' : `${failures} check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
