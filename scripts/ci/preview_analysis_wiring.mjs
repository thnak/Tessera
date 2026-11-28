// SPDX-License-Identifier: MIT
//
// The editor wiring of the analysis service (#984, #988): the answers as page data (definition target, reference
// rows, hover card, completion items, rename plan), the hub the commands and the CodeMirror extensions share (F12,
// Shift+F12, F2 with one multi-file undo step, hover and completion that stay quiet without a worker), the client's
// position request (an unchanged project is not resent; a missing engine marks the features unavailable) and the
// static wiring (live commands, handlers, the editor extensions). Modules are imported from dist-ssr (\`npm run
// build:chrome\` is a prerequisite; a missing build is exit code 2). With TESSERA_ANALYSIS_WASM set to a built
// tessera_analysis.js the hub runs against the C++ library through the page's own worker source.
//
//   node scripts/ci/preview_analysis_wiring.mjs

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { Worker } from 'node:worker_threads';

import { analysisServiceSource, wasmEngineSource } from '../../designer/web/services/analysis-service.js';
import { assert, finish, test } from './mock_test_support.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const DIST = resolve(ROOT, 'designer', 'web', 'dist-ssr');
let nav;
let hubModule;
let clientModule;
try {
  nav = await import(pathToFileURL(resolve(DIST, 'analysis-nav.js')).href);
  hubModule = await import(pathToFileURL(resolve(DIST, 'analysis-hub.js')).href);
  clientModule = await import(pathToFileURL(resolve(DIST, 'analysis-client.js')).href);
} catch {
  process.stderr.write('preview_analysis_wiring: ' + DIST + ' is missing; run npm run build:chrome --prefix designer/web first\n');
  process.exit(2);
}

const emoji = '\u{1F600}';
const read = (path) => readFileSync(resolve(ROOT, path), 'utf8');
const ENTRIES = [
  [0, 'import { Card } from "./Card";\nexport const App = () => <Card />;\n', 'App.tsx'],
  [1, '// ' + emoji + '\nexport function Card() { return <p />; }\n', 'Card.tsx'],
];

// --- the answers as page data --------------------------------------------------------------------------

await test('nav: a definition is a path and a span; no declaration says why, in words', () => {
  const found = nav.definitionResult({ definitions: [{ file: 1, from: 20, to: 24 }] }, ENTRIES);
  assert.deepEqual(found.target, { path: 'Card.tsx', offset: 20, length: 4 });
  const none = nav.definitionResult({ definitions: [], unresolved: 'external' }, ENTRIES);
  assert.equal(none.target, null);
  assert.match(none.message, /outside the project/);
  assert.match(nav.definitionResult({ definitions: [] }, ENTRIES).message, /No definition/);
});

await test('nav: reference rows carry line, column in UTF-16 and the trimmed line', () => {
  const card = ENTRIES[1][1];
  const at = card.indexOf('Card');
  const rows = nav.referenceRows({ references: [{ file: 1, from: at, to: at + 4, kind: 'declaration' }, { file: 9, from: 0, to: 1, kind: 'x' }] }, ENTRIES);
  assert.equal(rows.length, 1, 'a file id outside the snapshot is dropped');
  assert.deepEqual([rows[0].path, rows[0].line, rows[0].column, rows[0].kind], ['Card.tsx', 2, 17, 'declaration']);
  assert.equal(rows[0].preview, 'export function Card() { return <p />; }');
});

await test('nav: the hover card is plain lines; nothing is markup', () => {
  assert.equal(nav.hoverCard({ hover: null }), null);
  const card = nav.hoverCard({ hover: { from: 3, to: 7, kind: 'component', name: 'Card', signature: 'function Card()', doc: '<b>x</b>', container: 'App', diagnostics: [{ code: 'C1', severity: 'warning', message: 'm' }] } });
  assert.equal(card.title, 'component Card in App');
  assert.equal(card.doc, '<b>x</b>', 'kept as text; the editor builds text nodes');
  assert.deepEqual(card.problems, ['warning: m (C1)']);
  assert.match(nav.hoverCard({ hover: { from: 0, to: 1, kind: 'cssClass', name: 'x', unresolved: 'external', diagnostics: [] } }).title, /outside the project/);
});

await test('nav: completion items map to editor types; an empty list is no completion', () => {
  assert.equal(nav.completionOptions({ from: 0, to: 0, context: 'none', items: [] }), null);
  const got = nav.completionOptions({ from: 2, to: 4, context: 'tag', items: [{ label: 'Card', kind: 'component' }, { label: 'Item', kind: 'import', detail: './Item' }, { label: 'x', kind: 'state' }] });
  assert.deepEqual(got.options.map((o) => o.type), ['class', 'namespace', 'variable']);
  assert.equal(got.options[1].detail, './Item');
  assert.deepEqual([got.from, got.to], [2, 4]);
});

await test('nav: a rename plan applies the edits per file; a refusal and a bad edit are sentences', () => {
  const app = ENTRIES[0][1];
  const edits = [
    { file: 0, offset: app.lastIndexOf('Card'), length: 4, newText: 'Tile' },
    { file: 0, offset: app.indexOf('Card'), length: 4, newText: 'Tile' },
    { file: 1, offset: ENTRIES[1][1].indexOf('Card'), length: 4, newText: 'Tile' },
  ];
  const plan = nav.renamePlan({ edits }, ENTRIES);
  assert.equal(plan.ok, true);
  assert.equal(plan.edits, 3);
  assert.deepEqual(plan.changes.map((c) => c.path).sort(), ['App.tsx', 'Card.tsx']);
  assert.equal(plan.changes.find((c) => c.path === 'App.tsx').after.includes('<Card'), false);
  assert.equal(nav.renamePlan({ edits: [], refusal: { code: 'X', message: 'because' } }, ENTRIES).message, 'because');
  assert.equal(nav.renamePlan({ edits: [{ file: 0, offset: 0, length: 9999, newText: '' }] }, ENTRIES).ok, false);
  assert.equal(nav.renamePlan({ edits: [] }, ENTRIES).ok, false);
});

await test('nav: entries take the open file from the editor and skip other kinds', () => {
  const project = { files: [{ path: 'a.tsx', kind: 'source' }, { path: 'f.ttf', kind: 'font' }, { path: 'b.css', kind: 'stylesheet' }], textOf: (p) => 'disk:' + p };
  const entries = nav.navEntries(project, { path: 'a.tsx', text: 'typed' });
  assert.deepEqual(entries, [[0, 'typed', 'a.tsx'], [1, 'disk:b.css', 'b.css']]);
});

// --- the hub, against a scripted client ------------------------------------------------------------------

function scripted(answers) {
  const calls = [];
  let available = true;
  return {
    calls,
    setAvailable: (value) => (available = value),
    problems: async () => null,
    available: () => available,
    dispose() {},
    async query(entries, type, payload) {
      calls.push({ type, payload, files: entries.length });
      const answer = answers[type];
      if (!available) return { ok: false, message: 'Code intelligence is not available on this site.' };
      return answer === undefined ? null : { ok: true, value: typeof answer === 'function' ? answer(entries, payload) : answer };
    },
  };
}

function page(texts, flags = {}) {
  const buffers = new Map(Object.entries(texts));
  const log = { opened: [], notices: [], picks: [], writes: [] };
  const project = {
    files: [...buffers.keys()].map((path) => ({ path, kind: path.endsWith('.css') ? 'stylesheet' : path.endsWith('.tsx') ? 'source' : 'text', editable: flags.locked !== path })),
    textOf: (path) => buffers.get(path) ?? '',
    openAt: (target) => log.opened.push(target),
    writeBuffers(writes) {
      log.writes.push(writes.map((w) => w.path));
      for (const w of writes) buffers.set(w.path, w.text);
    },
    notify: (notice) => log.notices.push(notice),
    pickReference: (rows) => log.picks.push(rows),
  };
  return { project, buffers, log };
}

const TEXTS = { 'App.tsx': ENTRIES[0][1], 'Card.tsx': ENTRIES[1][1] };
const at = (path, offset) => ({ path, text: TEXTS[path], offset });
const mk = (client, askName) => {
  const hub = hubModule.createAnalysisHub({ createClient: () => client, askName });
  return hub;
};

await test('hub: F12 opens the declaration; an unresolved name notifies instead', async () => {
  const client = scripted({ definition: (e, p) => (p.offset === 0 ? { definitions: [{ file: 1, from: 20, to: 24 }] } : { definitions: [], unresolved: 'builtin' }) });
  const hub = mk(client);
  const p = page(TEXTS);
  hub.attach(p.project);
  await hub.definition(at('App.tsx', 0));
  assert.deepEqual(p.log.opened, [{ path: 'Card.tsx', offset: 20, length: 4 }]);
  assert.deepEqual(client.calls[0].payload, { file: 0, offset: 0 });
  await hub.definition(at('App.tsx', 5));
  assert.equal(p.log.opened.length, 1);
  assert.match(p.log.notices.at(-1).message, /built in/);
});

await test('hub: Shift+F12 opens a single reference, offers several, and says when there are none', async () => {
  const refs = [{ file: 0, from: 9, to: 13, kind: 'import' }, { file: 1, from: 20, to: 24, kind: 'declaration' }];
  let answer = { references: refs };
  const client = scripted({ references: () => answer });
  const hub = mk(client);
  const p = page(TEXTS);
  hub.attach(p.project);
  await hub.references(at('App.tsx', 9));
  assert.equal(p.log.picks.length, 1);
  assert.equal(p.log.picks[0].length, 2);
  assert.equal(client.calls[0].payload.includeDeclaration, true);
  answer = { references: [refs[1]] };
  await hub.references(at('App.tsx', 9));
  assert.equal(p.log.opened.length, 1);
  answer = { references: [] };
  await hub.references(at('App.tsx', 9));
  assert.match(p.log.notices.at(-1).message, /No references/);
});

function renameClient(texts) {
  const app = texts['App.tsx'];
  const card = texts['Card.tsx'];
  return scripted({
    prepareRename: { prepare: { from: 9, to: 13, placeholder: 'Card', kind: 'component' } },
    rename: (entries, payload) => ({
      edits: [
        { file: 1, offset: card.indexOf('Card'), length: 4, newText: payload.newName },
        { file: 0, offset: app.lastIndexOf('Card'), length: 4, newText: payload.newName },
        { file: 0, offset: app.indexOf('Card'), length: 4, newText: payload.newName },
      ],
    }),
  });
}

await test('hub: F2 renames across files as one history entry, and one undo restores them all', async () => {
  const client = renameClient(TEXTS);
  const asked = [];
  const hub = mk(client, (current) => (asked.push(current), 'Tile'));
  const p = page(TEXTS);
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.deepEqual(asked, ['Card']);
  assert.equal(p.buffers.get('App.tsx').includes('<Card'), false);
  assert.equal(p.buffers.get('App.tsx').includes('{ Tile }'), true);
  assert.equal(p.buffers.get('Card.tsx').includes('function Tile()'), true);
  assert.equal(p.log.writes.length, 1, 'both files are written in one operation');
  assert.deepEqual(p.log.writes[0].sort(), ['App.tsx', 'Card.tsx']);
  const notice = p.log.notices.at(-1);
  assert.match(notice.message, /Rename Card to Tile \(3 places in 2 files\)/);
  assert.deepEqual(notice.actions, [{ label: 'Undo rename', command: 'edit.undoRename' }]);
  assert.equal(hub.canUndoRename(), true);
  hub.undoRename();
  assert.equal(p.buffers.get('App.tsx'), TEXTS['App.tsx']);
  assert.equal(p.buffers.get('Card.tsx'), TEXTS['Card.tsx']);
  assert.equal(p.log.writes.length, 2);
  assert.equal(hub.canUndoRename(), false);
});

await test('hub: undo leaves a file edited since alone and says so', async () => {
  const hub = mk(renameClient(TEXTS), () => 'Tile');
  const p = page(TEXTS);
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  p.buffers.set('Card.tsx', p.buffers.get('Card.tsx') + '// typed\n');
  hub.undoRename();
  assert.equal(p.buffers.get('App.tsx'), TEXTS['App.tsx']);
  assert.match(p.buffers.get('Card.tsx'), /typed/);
  assert.match(p.log.notices.at(-1).message, /Left alone because they changed since: Card.tsx/);
});

await test('hub: rename refuses a locked file, a refusal answer, a cancelled prompt and a file that moved on', async () => {
  let p = page(TEXTS, { locked: 'Card.tsx' });
  let hub = mk(renameClient(TEXTS), () => 'Tile');
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.match(p.log.notices.at(-1).message, /Card.tsx is locked/);
  assert.equal(p.log.writes.length, 0);

  const refusing = scripted({ prepareRename: { prepare: null, refusal: { code: 'TESSERA_ANALYSIS_RENAME_EXTERNAL', message: 'declared outside the project' } } });
  p = page(TEXTS);
  hub = mk(refusing, () => 'Tile');
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.match(p.log.notices.at(-1).message, /outside the project/);

  const client = renameClient(TEXTS);
  p = page(TEXTS);
  hub = mk(client, () => null);
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.equal(client.calls.some((c) => c.type === 'rename'), false, 'a cancelled prompt asks nothing more');

  const racing = renameClient(TEXTS);
  const original = racing.query;
  racing.query = async (entries, type, payload) => {
    const out = await original(entries, type, payload);
    if (type === 'rename') p.buffers.set('Card.tsx', 'changed\n');
    return out;
  };
  p = page(TEXTS);
  hub = mk(racing, () => 'Tile');
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.match(p.log.notices.at(-1).message, /changed while renaming/);
  assert.equal(hub.canUndoRename(), false);

  const refused = scripted({ prepareRename: { prepare: { from: 0, to: 4, placeholder: 'Card', kind: 'component' } }, rename: { edits: [], refusal: { code: 'TESSERA_ANALYSIS_RENAME_COLLISION', message: 'Tile already exists' } } });
  p = page(TEXTS);
  hub = mk(refused, () => 'Tile');
  hub.attach(p.project);
  await hub.rename(at('App.tsx', 9));
  assert.match(p.log.notices.at(-1).message, /Tile already exists/);
  assert.equal(p.log.writes.length, 0);
});

await test('hub: hover and completion are quiet without a project, a worker or a file the service reads', async () => {
  const client = scripted({ hover: { hover: { from: 0, to: 4, kind: 'component', name: 'Card', diagnostics: [] } }, completion: { from: 0, to: 0, context: 'tag', items: [{ label: 'Card', kind: 'component' }] } });
  const hub = mk(client);
  assert.equal(await hub.hover(at('App.tsx', 1)), null, 'no project attached');
  const p = page({ ...TEXTS, 'notes.txt': 'x' });
  hub.attach(p.project);
  assert.equal((await hub.hover(at('App.tsx', 1))).title, 'component Card');
  assert.equal((await hub.complete(at('App.tsx', 1))).options[0].label, 'Card');
  assert.equal(await hub.hover({ path: 'notes.txt', text: 'x', offset: 0 }), null);
  assert.equal(p.log.notices.length, 0, 'quiet requests never notify');
  client.setAvailable(false);
  assert.equal(await hub.hover(at('App.tsx', 1)), null);
  assert.equal(await hub.complete(at('App.tsx', 1)), null);
  assert.equal(p.log.notices.length, 0);
});

await test('hub: ready() turns false once the worker is unavailable, and listeners hear it once', async () => {
  const client = scripted({ definition: { definitions: [] } });
  const hub = mk(client);
  const p = page(TEXTS);
  hub.attach(p.project);
  let heard = 0;
  hub.subscribe(() => (heard += 1));
  assert.equal(hub.ready(), true);
  client.setAvailable(false);
  await hub.definition(at('App.tsx', 0));
  await hub.definition(at('App.tsx', 0));
  assert.equal(hub.ready(), false);
  assert.equal(heard, 1);
  assert.match(p.log.notices.at(-1).message, /not available/);
});

// --- the client's position request, against a stand-in engine ----------------------------------------------

function nodeServiceWorker(source, posts) {
  const shim = [
    'const { parentPort } = require("node:worker_threads");',
    'globalThis.postMessage = (m) => parentPort.postMessage(m);',
    'globalThis.addEventListener = (t, f) => parentPort.on("message", (d) => f({ data: d }));',
  ].join('\n');
  const worker = new Worker(shim + '\n' + source, { eval: true });
  return {
    post: (m, transfer = []) => (posts?.push(m.type), worker.postMessage(m, transfer)),
    onMessage: (f) => worker.on('message', f),
    onError: (f) => worker.on('error', (e) => f(String((e && e.message) || e))),
    terminate: () => void worker.terminate(),
  };
}

const ECHO = String.raw`
let project = new Map();
function tesseraAnalysisSnapshot(files) { project = new Map(files.map((f) => [f.file, f.text])); }
function tesseraAnalysisEdits() { return true; }
function tesseraAnalysisQuery(type, file, offset, flag, text) { return JSON.stringify({ echo: [type, file, offset, text === undefined ? null : text] }); }
`;
const BROKEN = String.raw`
function tesseraAnalysisSnapshot() {}
function tesseraAnalysisEdits() { return true; }
function tesseraAnalysisQuery() { throw new Error('no module'); }
`;

await test('client: an unchanged project is not sent again; a changed one is', async () => {
  const posts = [];
  const client = clientModule.createAnalysisClient(() => nodeServiceWorker(analysisServiceSource(ECHO), posts));
  const a = await client.query(ENTRIES, 'hover', { file: 0, offset: 3 });
  assert.deepEqual(a.value.echo, ['hover', 0, 3, null]);
  const b = await client.query(ENTRIES, 'definition', { file: 0, offset: 4 });
  assert.deepEqual(b.value.echo, ['definition', 0, 4, null]);
  assert.equal(posts.filter((t) => t === 'snapshot').length, 1);
  const changed = ENTRIES.map((e) => (e[0] === 0 ? [0, e[1] + ' ', e[2]] : e));
  await client.query(changed, 'hover', { file: 0, offset: 3 });
  assert.equal(posts.filter((t) => t === 'snapshot').length, 2);
  const renamed = await client.query(changed, 'rename', { file: 0, offset: 3, newName: 'Tile' });
  assert.deepEqual(renamed.value.echo, ['rename', 0, 3, 'Tile']);
  assert.equal(client.available(), true);
  client.dispose();
});

await test('client: an engine that cannot answer marks the features unavailable, without another worker round trip', async () => {
  const posts = [];
  const client = clientModule.createAnalysisClient(() => nodeServiceWorker(analysisServiceSource(BROKEN), posts));
  const first = await client.query(ENTRIES, 'hover', { file: 0, offset: 0 });
  assert.equal(first.ok, false);
  assert.equal(client.available(), false);
  const before = posts.length;
  const second = await client.query(ENTRIES, 'hover', { file: 0, offset: 0 });
  assert.equal(second.ok, false);
  assert.equal(posts.length, before);
  client.dispose();
});

await test('client: a position in a file the snapshot lacks is a sentence, not unavailability', async () => {
  const client = clientModule.createAnalysisClient(() => nodeServiceWorker(analysisServiceSource(ECHO)));
  const out = await client.query(ENTRIES, 'hover', { file: 7, offset: 0 });
  assert.equal(out.ok, false);
  assert.equal(client.available(), true);
  client.dispose();
});

// --- static wiring -----------------------------------------------------------------------------------------

await test('wiring: the commands are live, their handlers and the editor extensions exist, the readiness key is published', () => {
  const commands = JSON.parse(read('designer/web/src/workbench/commands.json'));
  const list = Array.isArray(commands) ? commands : commands.commands;
  for (const id of ['go.definition', 'go.references', 'edit.renameSymbol', 'edit.undoRename']) {
    assert.equal(list.find((c) => c.id === id)?.status, 'live', id);
  }
  assert.equal(list.find((c) => c.id === 'go.definition').keybinding.default, 'F12');
  assert.equal(list.find((c) => c.id === 'go.references').keybinding.default, 'Shift+F12');
  assert.equal(list.find((c) => c.id === 'edit.renameSymbol').keybinding.default, 'F2');
  const target = read('designer/web/src/editor/editor-target.ts');
  for (const id of ['go.definition', 'go.references', 'edit.renameSymbol', 'edit.undoRename']) {
    assert.match(target, new RegExp('"' + id.replace('.', '\\.') + '":'), id);
  }
  assert.match(read('designer/web/src/editor/code-editor.ts'), /analysisExtensions\(analysisHub/);
  const extensions = read('designer/web/src/editor/analysis-extensions.ts');
  assert.match(extensions, /hoverTooltip\(/);
  assert.match(extensions, /autocompletion\(/);
  assert.equal(/innerHTML/.test(extensions), false, 'hover text is never parsed as markup');
  assert.match(read('designer/web/src/page-analysis.ts'), /"service\.analysis\.ready"/);
  assert.match(read('designer/web/src/App.tsx'), /useAnalysisHub\(/);
  assert.match(read('designer/web/src/regions/CommandPalette.tsx'), /intent === "references"/);
});

// --- the C++ library, when a Wasm module is built ----------------------------------------------------------

const wasm = process.env.TESSERA_ANALYSIS_WASM;
if (wasm) {
  const engine = wasmEngineSource(pathToFileURL(wasm).href);
  const card = '/** A card. */\nexport function Card({ title }) { return <p>{title}</p>; }\n';
  const app = '// ' + emoji + '\nimport { Card } from "./Card";\nexport function App() { return <Card title="a" />; }\n';
  const texts = { 'App.tsx': app, 'Card.tsx': card };
  const real = () => clientModule.createAnalysisClient(() => nodeServiceWorker(analysisServiceSource(engine)));

  await test('tessera_analysis.wasm: F12, references, hover and completion through the hub', async () => {
    const hub = hubModule.createAnalysisHub({ createClient: real });
    const p = page(texts);
    hub.attach(p.project);
    const tag = app.indexOf('<Card') + 2;
    await hub.definition({ path: 'App.tsx', text: app, offset: tag });
    assert.equal(p.log.opened[0].path, 'Card.tsx');
    assert.equal(card.slice(p.log.opened[0].offset, p.log.opened[0].offset + p.log.opened[0].length), 'Card');
    await hub.references({ path: 'Card.tsx', text: card, offset: card.indexOf('Card(') + 1 });
    assert.equal(p.log.picks[0].length, 3);
    const hover = await hub.hover({ path: 'App.tsx', text: app, offset: tag });
    assert.equal(hover.doc, 'A card.');
    assert.equal(app.slice(hover.from, hover.to), 'Card');
    const open = '// ' + emoji + '\nimport { Card } from "./Card";\nexport function App() { return <Ca'
    const done = await hub.complete({ path: 'App.tsx', text: open, offset: open.length });
    assert.equal(done.options.some((o) => o.label === 'Card'), true);
    assert.equal(open.slice(done.from, done.to), 'Ca');
    hub.dispose();
  });

  await test('tessera_analysis.wasm: F2 renames a component across files and one undo restores both', async () => {
    const hub = hubModule.createAnalysisHub({ createClient: real, askName: () => 'Tile' });
    const p = page(texts);
    hub.attach(p.project);
    await hub.rename({ path: 'App.tsx', text: app, offset: app.indexOf('<Card') + 2 });
    assert.equal(p.buffers.get('Card.tsx').includes('function Tile('), true);
    assert.equal(p.buffers.get('App.tsx').includes('<Card'), false);
    assert.equal(p.log.writes.length, 1);
    hub.undoRename();
    assert.equal(p.buffers.get('App.tsx'), app);
    assert.equal(p.buffers.get('Card.tsx'), card);
    hub.dispose();
  });
} else {
  process.stdout.write('skip wasm cases (set TESSERA_ANALYSIS_WASM to a built tessera_analysis.js)\n');
}

finish('preview_analysis_wiring');
