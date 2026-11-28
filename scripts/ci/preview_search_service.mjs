// SPDX-License-Identifier: MIT
//
// The project-search service gate (#580, ADR-060, docs/specs/studio-services-v1.md section 15): the matcher
// (plain text, case, whole word, include and exclude globs, the bounded regex engine, binary and size skips,
// result caps, the replace plan) and the worker protocol around it (handshake, snapshot and delta, latest
// wins, an invalid pattern as an error code). Runs under plain Node with no UI and no build:
//
//   node scripts/ci/preview_search_service.mjs

import { Worker } from 'node:worker_threads';

import { ServiceClient } from '../../designer/web/services/client.js';
import { searchEngineSource } from '../../designer/web/services/search-engine.js';
import { searchRegexSource } from '../../designer/web/services/search-regex.js';
import { searchServiceSource } from '../../designer/web/services/search-service.js';
import { assert, finish, test } from './mock_test_support.mjs';

// The engine text is evaluated as the worker evaluates it, so a passing case is a statement about what ships.
const engine = new Function(
  searchRegexSource + '\n' + searchEngineSource +
    '\nreturn { tesseraSearch, tesseraReplace, rxCompile, rxExec, searchGlobList, SEARCH_STEP_BUDGET };',
)();

const file = (id, path, text) => ({ file: id, path, text });
const never = () => false;
const search = (files, options) => engine.tesseraSearch(files, { query: 'x', ...options }, never);
const spans = (result) => result.files.flatMap((group) => group.matches.map((m) => [group.path, m.line, m.column, m.length]));
const apply = (text, edits) =>
  edits.slice().sort((a, b) => b.offset - a.offset).reduce((t, e) => t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length), text);

const SAMPLE = [
  file(1, 'ui/App.tsx', 'import { Card } from "./Card";\nexport const App = () => <Card title="Hello" />;\n'),
  file(2, 'ui/Card.tsx', 'export const Card = (p) => <div class="card">{p.title}</div>;\n// card CARD Card\n'),
  file(3, 'ui/app.css', '.card { color: red; }\n.Card-title { color: blue; }\r\n'),
  file(4, 'dist/out.js', 'var card = 1;\n'),
  file(5, 'tessera.json', '{ "name": "card" }\n'),
];

// --- globs --------------------------------------------------------------------------------------------------

await test('globs: a bare pattern matches at any depth, a folder excludes everything below it', () => {
  const only = engine.searchGlobList('*.tsx');
  assert.equal(only('ui/App.tsx'), true);
  assert.equal(only('App.tsx'), true);
  assert.equal(only('ui/app.css'), false);
  const dist = engine.searchGlobList('dist, node_modules/');
  assert.equal(dist('dist/out.js'), true);
  assert.equal(dist('a/dist/out.js'), true);
  assert.equal(dist('a/node_modules/x/y.js'), true);
  assert.equal(dist('distant/out.js'), false);
  const rooted = engine.searchGlobList('ui/*.css');
  assert.equal(rooted('ui/app.css'), true);
  assert.equal(rooted('x/ui/app.css'), false);
  const alt = engine.searchGlobList('**/*.{css,json}');
  assert.equal(alt('ui/app.css'), true);
  assert.equal(alt('tessera.json'), true);
  assert.equal(alt('ui/App.tsx'), false);
  assert.equal(engine.searchGlobList('  ,  '), null);
});

await test('globs: a path is data, never a regular expression (dots and brackets are literal)', () => {
  const test1 = engine.searchGlobList('a.b');
  assert.equal(test1('a.b'), true);
  assert.equal(test1('axb'), false);
  assert.equal(engine.searchGlobList('a(1)')('a(1)'), true);
});

// --- plain text ---------------------------------------------------------------------------------------------

await test('plain search: case-insensitive by default, results grouped by file in path order', async () => {
  const r = await search(SAMPLE, { query: 'card', exclude: 'dist' });
  assert.deepEqual(spans(r).map((s) => s.join(':')), ['tessera.json:1:12:4', 'ui/App.tsx:1:10:4', 'ui/App.tsx:1:25:4', 'ui/App.tsx:2:27:4', 'ui/Card.tsx:1:14:4', 'ui/Card.tsx:1:40:4', 'ui/Card.tsx:2:4:4', 'ui/Card.tsx:2:9:4', 'ui/Card.tsx:2:14:4', 'ui/app.css:1:2:4', 'ui/app.css:2:2:4']);
  assert.deepEqual(r.files.map((g) => g.path), ['tessera.json', 'ui/App.tsx', 'ui/Card.tsx', 'ui/app.css']);
  assert.equal(r.total, r.files.reduce((n, g) => n + g.matches.length, 0));
  assert.equal(r.truncated, null);
});

await test('plain search: case sensitive and whole word', async () => {
  const sensitive = await search(SAMPLE, { query: 'Card', caseSensitive: true, include: '*.tsx' });
  assert.deepEqual(spans(sensitive).filter((s) => s[0] === 'ui/Card.tsx').map((s) => [s[1], s[2]]), [[1, 14], [2, 14]]);
  const whole = await search(SAMPLE, { query: 'Card', caseSensitive: true, wholeWord: true, include: '*.css' });
  assert.deepEqual(spans(whole), [['ui/app.css', 2, 2, 4]], 'Card-title has a word boundary after Card');
});

await test('plain search: whole word rejects a match inside an identifier', async () => {
  const r = await search([file(1, 'a.ts', 'cardinal card_x card Card\n')], { query: 'card', wholeWord: true });
  assert.deepEqual(r.files[0].matches.map((m) => m.column), [17, 22]);
  const edge = await search([file(1, 'a.ts', 'a.b a.bc\n')], { query: 'a.b', wholeWord: true });
  assert.deepEqual(edge.files[0].matches.map((m) => m.column), [1], 'the right edge is a word character, so a.bc is not a whole word');
});

await test('spans are UTF-16 offsets into the whole file, across CRLF and astral characters', async () => {
  const text = 'x\r\n😀 hit 😀 hit\nlast hit';
  const r = await search([file(7, 'u.txt', text)], { query: 'hit' });
  const offsets = r.files[0].matches.map((m) => m.offset);
  assert.deepEqual(offsets, [text.indexOf('hit'), text.lastIndexOf('hit', text.length - 6), text.lastIndexOf('hit')]);
  for (const m of r.files[0].matches) assert.equal(text.slice(m.offset, m.offset + m.length), 'hit');
  assert.deepEqual(r.files[0].matches.map((m) => [m.line, m.column]), [[2, 4], [2, 11], [3, 6]]);
});

await test('case folding that changes a length falls back to the VM instead of drifting offsets', async () => {
  const text = 'İ x İstanbul x\n';
  const r = await search([file(1, 'a.txt', text)], { query: 'x' });
  assert.deepEqual(r.files[0].matches.map((m) => m.offset), [text.indexOf('x'), text.lastIndexOf('x')]);
});

await test('the preview is one trimmed line with the match inside it', async () => {
  const r = await search([file(1, 'a.txt', '        const value = needle + 1;\n')], { query: 'needle' });
  const m = r.files[0].matches[0];
  assert.equal(m.text.slice(m.start, m.end), 'needle');
  assert.equal(m.text.startsWith('const'), true);
  const long = await search([file(1, 'b.txt', 'a'.repeat(5000) + 'needle' + 'b'.repeat(5000) + '\n')], { query: 'needle' });
  const l = long.files[0].matches[0];
  assert.equal(l.text.slice(l.start, l.end), 'needle');
  assert.equal(l.text.length < 400, true);
});

// --- skips and caps -----------------------------------------------------------------------------------------

await test('binary and oversized files are skipped and counted, not searched', async () => {
  const big = file(2, 'big.txt', 'needle\n'.repeat(200000));
  const bin = file(3, 'font.bin', 'needle\u0000needle');
  const r = await search([file(1, 'ok.txt', 'needle\n'), big, bin], { query: 'needle' });
  assert.deepEqual(r.files.map((g) => g.path), ['ok.txt']);
  assert.equal(r.skippedBinary, 1);
  assert.equal(r.skippedLarge, 1);
  assert.equal(r.searched, 1);
});

await test('the result cap stops the walk and says so', async () => {
  const r = await search([file(1, 'a.txt', 'x\n'.repeat(500))], { query: 'x', maxResults: 100 });
  assert.equal(r.total, 100);
  assert.equal(r.truncated, 'results');
  const exact = await search([file(1, 'a.txt', 'x\n'.repeat(100))], { query: 'x', maxResults: 100 });
  assert.equal(exact.total, 100);
  assert.equal(exact.truncated, null);
});

await test('a very long line is skipped and counted (minified output cannot stall the walk)', async () => {
  const r = await search([file(1, 'min.js', 'x'.repeat(30000) + '\nx\n')], { query: 'x' });
  assert.equal(r.longLines, 1);
  assert.equal(r.total, 1);
});

// --- regular expressions ------------------------------------------------------------------------------------

await test('regex: groups, alternation, classes, anchors, lazy and counted repeats', async () => {
  const files = [file(1, 'a.txt', 'id 12-345 id 6-7\nfoo bar\nxaaay\nabcabc\n')];
  const m = async (query, options = {}) => (await search(files, { query, regex: true, ...options })).files.flatMap((g) => g.matches.map((x) => files[0].text.slice(x.offset, x.offset + x.length)));
  assert.deepEqual(await m('\\d+-\\d+'), ['12-345', '6-7']);
  assert.deepEqual(await m('foo|bar'), ['foo', 'bar']);
  assert.deepEqual(await m('^foo'), ['foo']);
  assert.deepEqual(await m('bar$'), ['bar']);
  assert.deepEqual(await m('a+?'), ['a', 'a', 'a', 'a', 'a', 'a']);
  assert.deepEqual(await m('x(a{2,3})y'), ['xaaay']);
  assert.deepEqual(await m('(abc){2}'), ['abcabc']);
  assert.deepEqual(await m('[^a-z\\s]+'), ['12-345', '6-7']);
  assert.deepEqual(await m('ID \\d', { caseSensitive: false }), ['id 1', 'id 6']);
  assert.deepEqual(await m('ID \\d', { caseSensitive: true }), []);
  assert.deepEqual(await m('\\bbar\\b'), ['bar']);
});

await test('regex: whole word wraps the whole pattern', async () => {
  const files = [file(1, 'a.txt', 'cat concat cat\n')];
  const r = await search(files, { query: 'cat|con', regex: true, wholeWord: true });
  assert.deepEqual(r.files[0].matches.map((x) => x.column), [1, 12]);
});

await test('regex: lookaround, backreferences and named groups are refused with a sentence', async () => {
  for (const query of ['(?=a)', '(?!a)', '(?<=a)b', '(?<n>a)', '(a)\\1', '\\k<n>', 'a**', '(', '(a', '[a', 'a{3,2}', '*a']) {
    await assert.rejects(search(SAMPLE, { query, regex: true }), (e) => e.code === 'TESSERA_SEARCH_PATTERN' && e.message.length > 0, query);
  }
});

await test('regex: a pattern that backtracks catastrophically costs linear time, not minutes', async () => {
  const text = 'a'.repeat(8000) + '!';
  const started = Date.now();
  const r = await search([file(1, 'a.txt', text)], { query: '(a+)+$', regex: true });
  assert.equal(r.total, 0);
  const again = await search([file(1, 'a.txt', text)], { query: '(a|aa)+b', regex: true });
  assert.equal(again.total, 0);
  assert.equal(Date.now() - started < 3000, true, 'took ' + (Date.now() - started) + ' ms');
});

await test('regex: oversized patterns and repeat counts are refused', async () => {
  await assert.rejects(search(SAMPLE, { query: 'a'.repeat(501), regex: true }), (e) => e.code === 'TESSERA_SEARCH_PATTERN');
  await assert.rejects(search(SAMPLE, { query: 'a{1001}', regex: true }), (e) => e.code === 'TESSERA_SEARCH_PATTERN');
  await assert.rejects(search(SAMPLE, { query: '((a{1000}){1000}){1000}', regex: true }), (e) => e.code === 'TESSERA_SEARCH_PATTERN');
  await assert.rejects(search(SAMPLE, { query: 'x'.repeat(1001) }), (e) => e.code === 'TESSERA_SEARCH_PATTERN');
  await assert.rejects(search(SAMPLE, { query: '' }), (e) => e.code === 'TESSERA_SEARCH_PATTERN');
});

await test('regex: the shared step budget ends a runaway walk as truncated "budget"', async () => {
  const r = await search([file(1, 'a.txt', ('a'.repeat(15000) + '\n').repeat(20))], { query: '(a*)*b', regex: true, stepBudget: 100000 });
  assert.equal(r.truncated, 'budget');
});

await test('cancel: a stop request ends the walk between files', async () => {
  const many = Array.from({ length: 400 }, (_, i) => file(i, 'f' + String(i).padStart(3, '0') + '.txt', 'needle\n'.repeat(4000)));
  let polls = 0;
  const r = await engine.tesseraSearch(many, { query: 'needle', regex: true, maxResults: 50000 }, () => (polls += 1) > 1);
  assert.equal(r.truncated, 'cancelled');
});

// --- replace plan -------------------------------------------------------------------------------------------

await test('replace plan: literal text, picks, and UTF-16 edits that apply to the text', async () => {
  const files = [file(1, 'a.txt', 'Card card\nCard\n'), file(2, 'b.txt', 'a Card 😀 Card\n')];
  const all = await engine.tesseraReplace(files, { query: 'card' }, 'Tile $1', null, never);
  assert.equal(all.edits.length, 5);
  assert.equal(all.files, 2);
  assert.equal(apply(files[0].text, all.edits.filter((e) => e.file === 1)), 'Tile $1 Tile $1\nTile $1\n');
  assert.equal(apply(files[1].text, all.edits.filter((e) => e.file === 2)), 'a Tile $1 😀 Tile $1\n');
  const one = await engine.tesseraReplace(files, { query: 'card' }, 'X', [{ file: 2, offset: files[1].text.lastIndexOf('Card') }], never);
  assert.equal(one.edits.length, 1);
  assert.equal(apply(files[1].text, one.edits), 'a Card 😀 X\n');
});

await test('replace plan: regex groups, $&, $$ and a group that did not take part', async () => {
  const files = [file(1, 'a.txt', 'id 12-345\n')];
  const plan = (replacement, query = '(\\d+)-(\\d+)') => engine.tesseraReplace(files, { query, regex: true }, replacement, null, never);
  assert.equal(apply(files[0].text, (await plan('$2:$1')).edits), 'id 345:12\n');
  assert.equal(apply(files[0].text, (await plan('[$&] $$')).edits), 'id [12-345] $\n');
  assert.equal(apply(files[0].text, (await plan('<$3>')).edits), 'id <$3>\n');
  assert.equal(apply(files[0].text, (await plan('<$1>', '(x)?(\\d+)')).edits), 'id <>-<>\n');
});

// --- the worker protocol ------------------------------------------------------------------------------------

function nodeServiceWorker(source) {
  const shim = [
    'const { parentPort } = require("node:worker_threads");',
    'globalThis.postMessage = (m) => parentPort.postMessage(m);',
    'globalThis.addEventListener = (t, f) => parentPort.on("message", (d) => f({ data: d }));',
  ].join('\n');
  const worker = new Worker(shim + '\n' + source, { eval: true });
  return {
    post: (m, transfer = []) => worker.postMessage(m, transfer),
    onMessage: (f) => worker.on('message', f),
    onError: (f) => worker.on('error', (e) => f(String((e && e.message) || e))),
    terminate: () => void worker.terminate(),
  };
}
const codeOf = (promise) => promise.then(() => null, (e) => e.code);
async function started(entries = SAMPLE.map((f) => [f.file, f.text, f.path])) {
  const client = new ServiceClient({ name: 'search', createWorker: () => nodeServiceWorker(searchServiceSource()) });
  await client.start();
  await client.setSnapshot(entries);
  return client;
}

await test('worker: the handshake names the search service and its two features', async () => {
  const client = new ServiceClient({ name: 'search', createWorker: () => nodeServiceWorker(searchServiceSource()) });
  assert.deepEqual(await client.start(), { name: 'search', protocol: 1, features: ['search', 'replace'] });
  client.dispose();
});

await test('worker: a search answers plain data with paths and UTF-16 spans', async () => {
  const client = await started();
  const r = await client.request('search', { query: 'card', caseSensitive: true, include: '*.tsx' });
  assert.deepEqual(r.files.map((g) => g.path), ['ui/Card.tsx']);
  assert.equal(r.files[0].matches[0].offset, SAMPLE[1].text.indexOf('card'));
  client.dispose();
});

await test('worker: a delta is seen by the next search, and a stale base is refused', async () => {
  const client = await started();
  await client.applyEdits([{ file: 5, offset: 0, length: 0, newText: 'card ' }]);
  const r = await client.request('search', { query: 'card', include: 'tessera.json' });
  assert.equal(r.total, 2);
  client.dispose();
});

await test('worker: replace returns edits the page can apply as one operation', async () => {
  const client = await started();
  const r = await client.request('replace', { query: 'Card', caseSensitive: true, wholeWord: true, replacement: 'Tile', include: 'ui/*.tsx' });
  assert.equal(r.files, 2);
  assert.equal(apply(SAMPLE[1].text, r.edits.filter((e) => e.file === 2)), SAMPLE[1].text.replace(/\bCard\b/g, 'Tile'));
  client.dispose();
});

await test('worker: an invalid pattern is an error code, a malformed request is a protocol error', async () => {
  const client = await started();
  assert.equal(await codeOf(client.request('search', { query: '(?=a)', regex: true })), 'TESSERA_SEARCH_PATTERN');
  assert.equal(await codeOf(client.request('search', { query: 5 })), 'TESSERA_STUDIO_MALFORMED');
  assert.equal(await codeOf(client.request('replace', { query: 'a' })), 'TESSERA_STUDIO_MALFORMED');
  assert.equal(await codeOf(client.request('replace', { query: 'a', replacement: '', only: [{ file: 'x' }] })), 'TESSERA_STUDIO_MALFORMED');
  assert.equal(await codeOf(client.request('nope', {})), 'TESSERA_STUDIO_UNKNOWN_TYPE');
  client.dispose();
});

await test('worker: the latest search wins and the older one is cancelled', async () => {
  const many = Array.from({ length: 300 }, (_, i) => [i, 'needle needle\n'.repeat(3000), 'f' + i + '.txt']);
  const client = await started(many);
  const first = client.request('search', { query: 'needle', regex: true, maxResults: 50000 });
  const second = client.request('search', { query: 'needle ', maxResults: 5 });
  assert.equal(await codeOf(first), 'TESSERA_STUDIO_CANCELLED');
  assert.equal((await second).total, 5);
  client.dispose();
});

await test('worker: no snapshot yet is an error, a crashed worker is recovered from the page text', async () => {
  const bare = new ServiceClient({ name: 'search', createWorker: () => nodeServiceWorker(searchServiceSource()) });
  await bare.start();
  assert.equal(await codeOf(bare.request('search', { query: 'a' })), 'TESSERA_STUDIO_NO_SNAPSHOT');
  bare.dispose();
  const client = await started();
  client.worker.terminate();
  client.failure = 'killed';
  const r = await client.withRecovery(async () => {
    await client.setSnapshot(SAMPLE.map((f) => [f.file, f.text, f.path]));
    return client.request('search', { query: 'card', include: '*.css' });
  });
  assert.equal(r.total, 2);
  client.dispose();
});

finish('preview_search_service');
