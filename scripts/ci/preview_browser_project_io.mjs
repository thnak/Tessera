// SPDX-License-Identifier: MIT
//
// Browser check of the studio's project load and download (issue #391, D-S4): a zip is opened, the tree
// lists it, a locked file is shown and never read, the entry previews, an edit survives Download, and a
// hostile or over-limit archive is refused with a named message.
//
//   node scripts/ci/preview_browser_project_io.mjs [site-dir]
//
// The folder picker needs a user gesture and a real directory, so that route is verified by hand; this
// probe drives the zip route, which Playwright can. Not a repository test: run it from a directory
// whose node_modules has playwright.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { chromium } from 'playwright';
import { writeZip, readZip } from '../../designer/web/studio-zip.js';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
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
    response.end('not found');
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=swiftshader'],
});
const page = await (await browser.newContext({ acceptDownloads: true })).newPage();
const consoleErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => consoleErrors.push(`pageerror: ${String(error)}`));
// Opening a project over unsaved edits asks first; the probe answers yes and records that it asked.
let confirmations = 0;
let promptAnswer = '';
let confirmAnswer = true;
page.on('dialog', (dialog) => {
  confirmations += 1;
  if (dialog.type() === 'prompt') {
    void dialog.accept(promptAnswer);
  } else if (confirmAnswer) {
    void dialog.accept();
  } else {
    void dialog.dismiss();
  }
});
await page.goto(base, { waitUntil: 'load' });

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? '  -- ' + detail : ''}`);
  if (!ok) failures += 1;
}

const encoder = new TextEncoder();
const zipOf = (files) =>
  Buffer.from(
    writeZip(
      Object.entries(files).map(([path, content]) => ({
        path,
        bytes: typeof content === 'string' ? encoder.encode(content) : content,
      })),
    ),
  );
const open = (files) =>
  page.locator('input[type=file]').setInputFiles({
    name: 'my-app.zip',
    mimeType: 'application/zip',
    buffer: zipOf(files),
  });
const note = async () => ((await page.locator('#project-note').textContent()) ?? '').trim();
const paths = () =>
  page.locator('#project-tree [data-path]').evaluateAll((nodes) => nodes.map((n) => n.dataset.path));

const SOURCE = `export function Home(): Node {
  return (
    <View className="page">
      <Text className="title">Opened from a zip</Text>
    </View>
  );
}
`;
const PROJECT = {
  'tessera.json':
    '{"schemaVersion":1,"app":{"name":"zip-demo","entry":"ui/Home.tsx","namespace":"zip_demo"},"assets":{"fonts":["fonts/Inter.ttf"]}}',
  'ui/Home.tsx': `import "../styles/app.css";\n\n${SOURCE}`,
  'styles/app.css': '.page { background-color: rgb(20, 160, 60); width: 320px; height: 240px; }\n',
  'fonts/Inter.ttf': new Uint8Array([0, 1, 2, 3]),
  'src/native.cpp': '// never edited',
};

await page.locator('#tab-canvas').click();
await page.waitForTimeout(600);

// --- 1. open a zip -----------------------------------------------------------------------------------
await open(PROJECT);
await page.waitForSelector('#project-name');
const tree = await paths();
check(
  'the tree lists the opened project, not the seed',
  tree.includes('ui/Home.tsx') && !tree.includes('ui/App.tsx'),
  tree.join(', '),
);
check('a font and C++ are listed locked', (await page.locator('.tree-row.is-locked').count()) >= 2);
check('the header names the archive', ((await page.locator('#project-name').textContent()) ?? '').includes('my-app'));
await page.waitForTimeout(2500);
const status = ((await page.locator('#canvas-status').textContent()) ?? '').trim();
check('the opened project previews (its entry, not a fallback)', /draw call/.test(status), status);

// --- 1b. the entry's components come from the compiler, and picking one previews it ------------------
const TWO = {
  'tessera.json': '{"schemaVersion":1,"app":{"name":"two","entry":"ui/App.tsx","namespace":"two"}}',
  'ui/App.tsx': `export function Home(): Node {
  return (<View><Text>Home</Text></View>);
}
export function Settings(): Node {
  return (<View><Text>One</Text><Text>Two</Text><Text>Three</Text></View>);
}
`,
};
await open(TWO);
await page.waitForSelector('#component-select');
await page.waitForTimeout(2500);
const options = await page.locator('#component-select option').allTextContents();
check('the dropdown lists the entry components in source order', options.join(',') === 'Home,Settings', options.join(','));
check('the first component is previewed by default', (await page.locator('#component-select').inputValue()) === 'Home');
const homeNodes = (await page.locator('#stat-nodes').textContent())?.trim();
await page.locator('#component-select').selectOption('Settings');
await page.waitForFunction((before) => document.getElementById('stat-nodes')?.textContent?.trim() !== before, homeNodes);
check('picking a component previews it', (await page.locator('#stat-nodes').textContent())?.trim() !== homeNodes, `${homeNodes} -> ${(await page.locator('#stat-nodes').textContent())?.trim()}`);
await open(PROJECT);
await page.waitForFunction(() => document.getElementById('component-select') === null);
check('a one-component project shows no dropdown', true);
await page.waitForTimeout(2000);

// --- 2. edit, then Download keeps the edit and omits locked files -------------------------------------
await page.locator('.tree-row[data-path="ui/Home.tsx"]').click();
await page.waitForFunction(() => window.__tesseraEditor?.text().includes('Opened from a zip'));
await page.evaluate(() => {
  const editor = window.__tesseraEditor;
  editor.edit(editor.text().replace('Opened from a zip', 'Edited in the studio'));
});
check(
  'the edited file is marked dirty',
  (await page.locator('.tree-row[data-path="ui/Home.tsx"] .tree-dirty').count()) === 1,
);
const [download] = await Promise.all([
  page.waitForEvent('download'),
  page.locator('#download-project').click(),
]);
const exported = await readZip(new Uint8Array(await readFile(await download.path())));
const byPath = Object.fromEntries(exported.map((entry) => [entry.path, new TextDecoder().decode(entry.bytes)]));
check('the download is named for the project', download.suggestedFilename() === 'my-app.zip', download.suggestedFilename());
check('the download carries the edit', (byPath['ui/Home.tsx'] ?? '').includes('Edited in the studio'));
check('the download carries every accepted file', 'tessera.json' in byPath && 'styles/app.css' in byPath);
check('the download omits locked files', !('fonts/Inter.ttf' in byPath) && !('src/native.cpp' in byPath));

// --- 3. a zip project has no folder to save into -----------------------------------------------------
check('Save is disabled for a zip project', await page.locator('#save-project').isDisabled());

// --- 4. refusals are named ------------------------------------------------------------------------------
const many = {};
for (let i = 0; i < 501; i += 1) many[`ui/F${i}.tsx`] = 'x';
await open(many);
await page.waitForFunction(() => /more than 500/.test(document.getElementById('project-note')?.textContent ?? ''));
check('501 editable files are refused by name', /more than 500/.test(await note()), await note());
check('opening over an unsaved edit asked first', confirmations >= 1, String(confirmations));
check('a refused open leaves the previous project', (await paths()).includes('ui/Home.tsx'));

await open({ 'tessera.json': '{}', '../escape.tsx': 'x' });
await page.waitForFunction(() => /\.\./.test(document.getElementById('project-note')?.textContent ?? ''));
check('a path that escapes the project is refused', /\.\./.test(await note()), await note());


// --- 5. the folder route, through a real FileSystemDirectoryHandle ---------------------------------------
// The picker needs a user gesture, so it is stubbed to return a directory of the browser's origin-private
// file system: a genuine handle (real `entries`, `getFile`, `createWritable`, `lastModified`), so Save and
// the conflict rule run against the platform and not a fake. The picker itself is checked by hand.
await page.evaluate(async (files) => {
  const root = await navigator.storage.getDirectory();
  try {
    await root.removeEntry('proj', { recursive: true });
  } catch {
    /* absent */
  }
  const dir = await root.getDirectoryHandle('proj', { create: true });
  for (const [path, text] of Object.entries(files)) {
    let at = dir;
    const parts = path.split('/');
    for (const part of parts.slice(0, -1)) at = await at.getDirectoryHandle(part, { create: true });
    const handle = await at.getFileHandle(parts.at(-1), { create: true });
    const writable = await handle.createWritable();
    await writable.write(text);
    await writable.close();
  }
  window.showDirectoryPicker = async () => dir;
}, { 'tessera.json': PROJECT['tessera.json'], 'ui/Home.tsx': PROJECT['ui/Home.tsx'], 'styles/app.css': PROJECT['styles/app.css'] });
const diskText = (path) =>
  page.evaluate(async (target) => {
    let at = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
    const parts = target.split('/');
    for (const part of parts.slice(0, -1)) at = await at.getDirectoryHandle(part);
    return (await (await at.getFileHandle(parts.at(-1))).getFile()).text();
  }, path);

await page.locator('#open-folder').click();
await page.waitForFunction(() => /Folder: proj/.test(document.getElementById('project-name')?.textContent ?? ''));
check('a folder opens and is named', true);
check('Save is enabled for a folder project', !(await page.locator('#save-project').isDisabled()));

await page.locator('.tree-row[data-path="ui/Home.tsx"]').click();
const editHome = async (from, to) => {
  await page.waitForFunction((needle) => window.__tesseraEditor?.text().includes(needle), from);
  await page.evaluate(([a, b]) => {
    const editor = window.__tesseraEditor;
    editor.edit(editor.text().replace(a, b));
  }, [from, to]);
};
await editHome('Opened from a zip', 'Saved to the folder');
await page.locator('#source').press('Control+s');
await page.waitForFunction(() => /saved/.test(document.getElementById('project-note')?.textContent ?? ''));
check('Ctrl+S writes the edit to the folder', (await diskText('ui/Home.tsx')).includes('Saved to the folder'));
check('the dirty mark clears after Save', (await page.locator('.tree-row[data-path="ui/Home.tsx"] .tree-dirty').count()) === 0);

// Another editor changes the file; a save must not overwrite it silently.
await new Promise((resolve) => setTimeout(resolve, 20));
await page.evaluate(async () => {
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
  const handle = await (await dir.getDirectoryHandle('ui')).getFileHandle('Home.tsx');
  const writable = await handle.createWritable();
  await writable.write('// changed by another editor');
  await writable.close();
});
await editHome('Saved to the folder', 'Mine again');
await page.locator('#save-project').click();
await page.waitForSelector('#project-conflict');
check('a file changed on disk raises the conflict choice', true);
check('nothing was written before the choice', (await diskText('ui/Home.tsx')) === '// changed by another editor');
await page.getByRole('button', { name: 'Cancel' }).click();
check('Cancel leaves the disk alone', (await diskText('ui/Home.tsx')) === '// changed by another editor');
await page.locator('#save-project').click();
await page.waitForSelector('#project-conflict');
await page.getByRole('button', { name: 'Keep mine' }).click();
await page.waitForFunction(() => /saved/.test(document.getElementById('project-note')?.textContent ?? ''));
check('Keep mine overwrites with the editor text', (await diskText('ui/Home.tsx')).includes('Mine again'));

// --- 6. new file, rename, delete: all pending until Save, nothing written before ---------------------
const onDisk = (path) =>
  page.evaluate(async (target) => {
    try {
      let at = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
      const parts = target.split('/');
      for (const part of parts.slice(0, -1)) at = await at.getDirectoryHandle(part);
      await at.getFileHandle(parts.at(-1));
      return true;
    } catch {
      return false;
    }
  }, path);
const waitNote = (pattern) =>
  page.waitForFunction((source) => new RegExp(source).test(document.getElementById('project-note')?.textContent ?? ''), pattern.source);

promptAnswer = 'ui/Extra.tsx';
await page.locator('#new-file').click();
await page.waitForSelector('.tree-row[data-path="ui/Extra.tsx"]');
check('New file appears in the tree, marked unsaved', (await page.locator('.tree-row[data-path="ui/Extra.tsx"] .tree-dirty').count()) === 1);
check('a new file is not on disk before Save', !(await onDisk('ui/Extra.tsx')));

promptAnswer = 'src/native.cpp';
await page.locator('#new-file').click();
await waitNote(/not a file the studio edits/);
check('a file type the studio does not edit is refused', (await page.locator('.tree-row[data-path="src/native.cpp"]').count()) === 0);
promptAnswer = '../escape.tsx';
await page.locator('#new-file').click();
await waitNote(/\.\./);
check('a path that escapes the project is refused as a new file', (await page.locator('.tree-row[data-path="../escape.tsx"]').count()) === 0);
promptAnswer = 'ui/Extra.tsx';
await page.locator('#new-file').click();
await waitNote(/already exists/);
check('a duplicate path is refused', true);

await page.locator('.tree-row[data-path="ui/Extra.tsx"]').click();
promptAnswer = 'ui/Renamed.tsx';
await page.locator('#rename-file').click();
await page.waitForSelector('.tree-row[data-path="ui/Renamed.tsx"]');
check('Rename moves the row and says imports are not rewritten', (await page.locator('.tree-row[data-path="ui/Extra.tsx"]').count()) === 0 && /not rewritten/.test(await note()), await note());

// An existing file that is deleted stays on disk until Save, and asks first.
await page.locator('.tree-row[data-path="styles/app.css"]').click();
confirmAnswer = false;
await page.locator('#delete-file').click();
check('declining the delete confirmation keeps the file', (await page.locator('.tree-row[data-path="styles/app.css"]').count()) === 1);
confirmAnswer = true;
await page.locator('#delete-file').click();
await page.waitForFunction(() => document.querySelector('.tree-row[data-path="styles/app.css"]') === null);
check('Delete drops the row, but the folder keeps the file until Save', await onDisk('styles/app.css'));

await page.locator('#save-project').click();
await waitNote(/deleted 1 file/);
check('Save creates the new file and deletes the removed one', (await onDisk('ui/Renamed.tsx')) && !(await onDisk('styles/app.css')) && !(await onDisk('ui/Extra.tsx')));
check('nothing is left unsaved after Save', (await page.locator('.tree-dirty').count()) === 0);

// --- 7. the last folder is remembered and offered back --------------------------------------------
await page.reload({ waitUntil: 'load' });
await page.waitForSelector('#reopen-folder');
check('after a reload the last folder is offered', /proj/.test(await page.locator('#reopen-folder').textContent()));
await page.evaluate(async () => {
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
  window.showDirectoryPicker = async () => dir;
});
await page.locator('#reopen-folder').click();
await page.waitForFunction(() => /Folder: proj/.test(document.getElementById('project-name')?.textContent ?? ''));
check('Reopen loads the remembered folder without the picker', (await page.locator('.tree-row[data-path="ui/Renamed.tsx"]').count()) === 1);

// --- 8. unsaved drafts survive a reload and are offered, never applied silently -----------------------
await page.locator('.tree-row[data-path="ui/Home.tsx"]').click();
await editHome('Mine again', 'Draft text');
await page.waitForTimeout(2600);
await page.reload({ waitUntil: 'load' });
await page.waitForSelector('#reopen-folder');
await page.evaluate(async () => {
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
  window.showDirectoryPicker = async () => dir;
});
await page.locator('#reopen-folder').click();
await page.waitForSelector('#project-draft');
check('the unsaved edit is offered back after a reload', /1 file/.test((await page.locator('#project-draft').textContent()) ?? ''));
check('the draft is not applied before the person chooses', !(await page.evaluate(() => window.__tesseraEditor.text())).includes('Draft text'));
await page.locator('#restore-draft').click();
await page.waitForFunction(() => /restored 1/.test(document.getElementById('project-note')?.textContent ?? ''));
await page.locator('.tree-row[data-path="ui/Home.tsx"]').click();
check('Restore brings the edit back, marked unsaved', (await page.evaluate(() => window.__tesseraEditor.text())).includes('Draft text') && (await page.locator('.tree-row[data-path="ui/Home.tsx"] .tree-dirty').count()) === 1);

// A file that changed on disk since the draft was kept is never overwritten by it.
await page.waitForTimeout(2600);
await page.evaluate(async () => {
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('proj');
  const handle = await (await dir.getDirectoryHandle('ui')).getFileHandle('Home.tsx');
  const writable = await handle.createWritable();
  await writable.write('// someone else rewrote it');
  await writable.close();
});
await page.reload({ waitUntil: 'load' });
await page.waitForSelector('#reopen-folder');
await page.locator('#reopen-folder').click();
await page.waitForFunction(() => /Folder: proj/.test(document.getElementById('project-name')?.textContent ?? ''));
await page.waitForTimeout(800);
check('a draft for a file that changed on disk is not offered', (await page.locator('#project-draft').count()) === 0);

check(
  'No console errors',
  consoleErrors.filter((text) => !/Failed to load resource/.test(text)).length === 0,
  consoleErrors.slice(0, 3).join(' | ') || 'none',
);
console.log(`\n${failures === 0 ? 'all project IO checks passed' : `${failures} check(s) failed`}`);
await browser.close();
server.close();
process.exit(failures === 0 ? 0 : 1);
