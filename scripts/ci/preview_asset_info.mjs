// SPDX-License-Identifier: MIT
//
// The asset editor's data layer (#581): image dimensions and formats, a font's family and license strings,
// the size budget hint, references to an asset in source and CSS, and the rename and replace plans. Runs
// under plain Node with synthetic bytes, no UI and no build:
//
//   node scripts/ci/preview_asset_info.mjs

import {
  assetClass, budgetHint, findAssetReferences, inspectAsset, planAssetRename, planAssetReplace, respecify,
  resolveSpecifier, svgSymbols,
} from '../../designer/web/asset-info.js';
import { assert, finish, test } from './mock_test_support.mjs';

const be32 = (n) => [(n >>> 24) & 255, (n >>> 16) & 255, (n >>> 8) & 255, n & 255];
const be16 = (n) => [(n >> 8) & 255, n & 255];
const le16 = (n) => [n & 255, (n >> 8) & 255];
const le32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255];
const str = (s) => [...s].map((c) => c.charCodeAt(0));
const bytes = (...parts) => Uint8Array.from(parts.flat());

const png = (w, h) => bytes([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a], be32(13), str('IHDR'), be32(w), be32(h), [8, 6, 0, 0, 0]);
const gif = (w, h) => bytes(str('GIF89a'), le16(w), le16(h), [0, 0, 0]);
const jpeg = (w, h) => bytes([0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0, 0xff, 0xc0, 0, 11, 8], be16(h), be16(w), [1, 1, 0x11, 0]);
const webpL = (w, h) => bytes(str('RIFF'), le32(20), str('WEBP'), str('VP8L'), le32(5), [0x2f], le32((w - 1) | ((h - 1) << 14)), [0]);
const webpX = (w, h) => bytes(str('RIFF'), le32(30), str('WEBP'), str('VP8X'), le32(10), [0, 0, 0, 0],
  [(w - 1) & 255, ((w - 1) >> 8) & 255, ((w - 1) >> 16) & 255, (h - 1) & 255, ((h - 1) >> 8) & 255, ((h - 1) >> 16) & 255]);
const ico = () => bytes(le16(0), le16(1), le16(2), [32, 32, 0, 0, 1, 0, 32, 0], le32(0), le32(0), [0, 0]);

/** A minimal TrueType file with a name table of Windows (UTF-16BE) records. */
function ttf(names) {
  const records = Object.entries(names);
  const strings = [];
  const recs = [];
  for (const [id, value] of records) {
    const data = [...value].flatMap((c) => be16(c.charCodeAt(0)));
    recs.push([...be16(3), ...be16(1), ...be16(0x409), ...be16(Number(id)), ...be16(data.length), ...be16(strings.length)]);
    strings.push(...data);
  }
  const header = [...be16(0), ...be16(records.length), ...be16(6 + records.length * 12)];
  const table = [...header, ...recs.flat(), ...strings];
  const dir = [...be32(0x00010000), ...be16(1), ...be16(16), ...be16(0), ...be16(0), ...str('name'), ...be32(0), ...be32(28), ...be32(table.length)];
  return bytes(dir, table);
}

await test('image formats report dimensions', () => {
  const cases = [['a.png', png(64, 32), 'png', 64, 32], ['a.gif', gif(10, 20), 'gif', 10, 20], ['a.jpg', jpeg(300, 200), 'jpeg', 300, 200],
    ['a.webp', webpL(120, 80), 'webp', 120, 80], ['b.webp', webpX(1000, 500), 'webp', 1000, 500], ['a.ico', ico(), 'ico', 32, 32]];
  for (const [path, data, format, width, height] of cases) {
    const info = inspectAsset(path, data);
    assert.deepEqual([info.class, info.format, info.width, info.height, info.bytes, info.warnings], ['image', format, width, height, data.length, []], path);
  }
});

await test('mismatched extension and unreadable images warn', () => {
  assert.match(inspectAsset('a.jpg', png(1, 1)).warnings[0], /content is png/);
  assert.match(inspectAsset('a.png', bytes([1, 2, 3])).warnings[0], /not a readable png/);
  assert.deepEqual(inspectAsset('a.png', bytes()).warnings.length, 1);
  assert.equal(assetClass('a.txt'), 'other');
});

await test('svg size from width/height or viewBox, and sprite symbols', () => {
  const enc = (s) => new TextEncoder().encode(s);
  assert.deepEqual(inspectAsset('i.svg', enc('<svg width="24px" height="16" xmlns="x"></svg>')).width, 24);
  const box = inspectAsset('i.svg', enc("<svg viewBox='0 0 48 32'><symbol id=\"home\"/><symbol class=\"a\" id='gear'/></svg>"));
  assert.deepEqual([box.width, box.height, box.symbols], [48, 32, ['home', 'gear']]);
  assert.deepEqual(svgSymbols('<svg></svg>'), []);
});

await test('font family and license come from the name table', () => {
  const font = ttf({ 0: 'Copyright 2024 Acme', 1: 'Acme Sans', 2: 'Bold', 13: 'Licensed under the OFL', 14: 'https://example.com/ofl' });
  const info = inspectAsset('fonts/acme.ttf', font);
  assert.deepEqual([info.class, info.format, info.family, info.style, info.copyright, info.license, info.licenseUrl],
    ['font', 'ttf', 'Acme Sans', 'Bold', 'Copyright 2024 Acme', 'Licensed under the OFL', 'https://example.com/ofl']);
  assert.deepEqual(info.warnings, []);
  assert.match(inspectAsset('x.ttf', ttf({ 1: 'NoLicense' })).warnings[0], /no license/);
  assert.match(inspectAsset('x.ttf', bytes(str('not a font at all'))).warnings[0], /not a readable font/);
  const woff = inspectAsset('x.woff2', bytes(str('wOF2'), new Array(20).fill(0)));
  assert.deepEqual([woff.format, woff.warnings], ['woff2', []]);
});

await test('truncated font data never throws', () => {
  const font = ttf({ 1: 'Cut' });
  for (let n = 0; n < font.length; n += 3) inspectAsset('x.ttf', font.subarray(0, n));
});

await test('budget hint levels', () => {
  assert.equal(budgetHint(100, 1000).level, 'ok');
  assert.equal(budgetHint(800, 1000).level, 'warn');
  assert.equal(budgetHint(1001, 1000).level, 'over');
  assert.match(budgetHint(2048, 1024).message, /over the 1\.0 KiB budget/);
});

await test('specifier resolution and respecify', () => {
  assert.equal(resolveSpecifier('ui/App.tsx', './logo.png'), 'ui/logo.png');
  assert.equal(resolveSpecifier('ui/App.tsx', '../assets/logo.png'), 'assets/logo.png');
  assert.equal(resolveSpecifier('ui/App.tsx', '../../x.png'), null);
  assert.equal(resolveSpecifier('ui/App.tsx', 'assets/logo.png'), 'assets/logo.png');
  assert.equal(respecify('ui/App.tsx', 'assets/new.png', '../assets/old.png'), '../assets/new.png');
  assert.equal(respecify('ui/App.tsx', 'ui/new.png', './old.png'), './new.png');
  assert.equal(respecify('ui/sub/A.tsx', 'assets/new.png', './old.png'), '../../assets/new.png');
  assert.equal(respecify('ui/App.tsx', 'assets/new.png', 'assets/old.png'), 'assets/new.png');
});

const FILES = [
  { path: 'ui/App.tsx', text: 'const a = <Image src="../assets/logo.png" />;\nconst b = "assets/logo.png";\nconst c = "https://x/assets/logo.png";\nconst d = "assets/logo.png.bak";' },
  { path: 'ui/app.css', text: '.a { background: url(../assets/logo.png); }\n.b { background: url("../assets/logo.png?v=2"); }\n.c { background: url(data:image/png;base64,AAAA); }' },
  { path: 'tessera.json', text: '{ "assets": { "images": ["assets/logo.png"] } }' },
  { path: 'notes.md', text: 'assets/logo.png' },
];
const PATHS = ['ui/App.tsx', 'ui/app.css', 'tessera.json', 'notes.md', 'assets/logo.png', 'assets/other.png'];

await test('references in strings, url() and config; remote and near-miss strings are ignored', () => {
  const refs = findAssetReferences(FILES, 'assets/logo.png');
  assert.deepEqual(refs.map((r) => [r.file, r.kind, FILES.find((f) => f.path === r.file).text.slice(r.offset, r.offset + r.length)]), [
    ['tessera.json', 'string', 'assets/logo.png'],
    ['ui/App.tsx', 'string', '../assets/logo.png'],
    ['ui/App.tsx', 'string', 'assets/logo.png'],
    ['ui/app.css', 'url', '../assets/logo.png'],
    ['ui/app.css', 'string', '../assets/logo.png'],
  ]);
});

await test('rename rewrites references in the original style', () => {
  const plan = planAssetRename(FILES, PATHS, 'assets/logo.png', 'assets/brand/mark.png');
  assert.equal(plan.ok, true);
  const out = new Map(FILES.map((f) => [f.path, f.text]));
  for (const e of plan.edits.slice().sort((a, b) => b.offset - a.offset)) {
    const t = out.get(e.file);
    out.set(e.file, t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length));
  }
  assert.match(out.get('ui/App.tsx'), /src="\.\.\/assets\/brand\/mark\.png"/);
  assert.match(out.get('ui/App.tsx'), /b = "assets\/brand\/mark\.png"/);
  assert.match(out.get('ui/App.tsx'), /assets\/logo\.png\.bak/);
  assert.match(out.get('ui/app.css'), /url\(\.\.\/assets\/brand\/mark\.png\)/);
  assert.match(out.get('ui/app.css'), /url\("\.\.\/assets\/brand\/mark\.png\?v=2"\)/);
  assert.match(out.get('tessera.json'), /"assets\/brand\/mark\.png"/);
  assert.equal(out.get('notes.md'), 'assets/logo.png');
  assert.equal(plan.references, plan.edits.length);
});

await test('rename refusals and warnings', () => {
  assert.match(planAssetRename(FILES, PATHS, 'assets/logo.png', 'assets/other.png').reason, /already taken/);
  assert.match(planAssetRename(FILES, PATHS, 'assets/logo.png', '../x.png').reason, /project-relative/);
  assert.match(planAssetRename(FILES, PATHS, 'assets/logo.png', '/x.png').reason, /project-relative/);
  assert.match(planAssetRename(FILES, PATHS, 'nope.png', 'x.png').reason, /not in this project/);
  assert.match(planAssetRename(FILES, PATHS, 'assets/logo.png', 'assets/logo.png').reason, /old path/);
  assert.match(planAssetRename(FILES, PATHS, 'assets/logo.png', 'assets/logo.jpg').warnings[0], /extension changes/);
});

await test('replace checks class, readability, limit and notices size changes', () => {
  assert.equal(planAssetReplace('a.png', png(10, 10), bytes(), 100).ok, false);
  assert.match(planAssetReplace('a.png', png(10, 10), png(10, 10), 5).reason, /over the 5 byte limit/);
  assert.match(planAssetReplace('a.png', png(10, 10), bytes([1, 2, 3, 4]), 100).reason, /not a readable/);
  assert.equal(planAssetReplace('a.txt', bytes([1]), bytes([1]), 100).ok, false);
  const same = planAssetReplace('a.png', png(10, 10), png(10, 10), 100);
  assert.deepEqual([same.ok, same.warnings], [true, []]);
  assert.match(planAssetReplace('a.png', png(10, 10), png(20, 5), 100).warnings[0], /10x10 to 20x5/);
  const fonts = planAssetReplace('f.ttf', ttf({ 1: 'A', 13: 'x' }), ttf({ 1: 'B', 13: 'x' }), 1000);
  assert.match(fonts.warnings[0], /family changes from A to B/);
});

finish('preview_asset_info');
