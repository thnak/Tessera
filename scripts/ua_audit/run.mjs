#!/usr/bin/env node
// SPDX-License-Identifier: MIT
/**
 * UA parity audit (issue #606, ADR-063): one markup per element, measured in Chromium and in
 * Tessera, and the difference reported per element. A difference is a bug unless it is a named
 * exception (docs/reference/ua-audit.md).
 *
 * Browser side: Playwright + Chromium, `getBoundingClientRect()` of the case's element, relative to
 * the wrapper. Tessera side: `tessera_scene_preview --scene-json` (the engine's own layout, native),
 * the first node under the wrapper. Both lay out at the same viewport, in CSS pixels.
 *
 * The browser page forces `line-height: 1.2` and the font of the engine's measurer is the stub's, so
 * a text line is 19.2px high on both sides: line height is a *font metric*, not a user-agent
 * default, and would otherwise read as a difference on every row. Everything else is left at the
 * browser's own default - that is the point of the audit.
 *
 *   node scripts/ua_audit/run.mjs --preview <tessera_scene_preview> [--chrome <chrome.exe>] [--out out/ua_audit] [--only h1,p]
 *
 * Playwright is not a repository dependency: set NODE_PATH to a directory that has it.
 */
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const arg = (n, d) => { const i = process.argv.indexOf(n); return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : d; };
const preview = arg('--preview', null);
const out = resolve(arg('--out', 'out/ua_audit'));
const chromePath = arg('--chrome', null);
const only = arg('--only', '').split(',').filter(Boolean);
if (!preview) { console.error('--preview <tessera_scene_preview> is required'); process.exit(2); }

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');
const fontUrl = pathToFileURL(join(here, '..', '..', 'tests', 'fixtures', 'fonts', 'NotoSans-Regular.ttf')).href;
const spec = JSON.parse(readFileSync(join(here, 'cases.json'), 'utf8'));
const [vw, vh] = spec.viewport;
mkdirSync(out, { recursive: true });

const browser = await chromium.launch(chromePath ? { executablePath: chromePath } : {});
const page = await browser.newPage({ viewport: { width: vw, height: vh } });

const round = (n) => Math.round(n * 100) / 100;
const rows = [];
for (const c of spec.cases) {
  if (only.length && !only.includes(c.id)) continue;
  await page.setContent(`<!doctype html><html><head><style>@font-face{font-family:T;src:url(${fontUrl})}html,body{margin:0;line-height:1.2;font-family:T}button,input,textarea{font-family:T;line-height:1.2}</style></head>` +
    `<body><div id="w" style="display:flow-root">${c.html}</div></body></html>`);
  const chrome = await page.evaluate(() => {
    const w = document.getElementById('w');
    const wr = w.getBoundingClientRect();
    const el = w.firstElementChild;
    const r = el.getBoundingClientRect();
    const cs = getComputedStyle(el);
    return { wrapperHeight: wr.height, x: r.x - wr.x, y: r.y - wr.y, width: r.width, height: r.height, display: cs.display,
             fontSize: parseFloat(cs.fontSize), fontWeight: cs.fontWeight };
  });

  const file = join(out, `${c.id}.tsx`);
  const sheet = join(out, 'Preview.css');
  writeFileSync(sheet, '/* the audit measures the user-agent sheet, not the preview fallback */\n:root { }\n');
  writeFileSync(file, `import "./Preview.css";\nexport function App() {\n  return <div>${c.tsx}</div>;\n}\n`);
  let json;
  try {
    json = JSON.parse(execFileSync(preview, ['--scene-json', '-', file, '--size', `${vw}x${vh}`, '--stylesheet', sheet, '--file', 'ui/Preview.tsx'],
      { encoding: 'utf8', maxBuffer: 1 << 24 }));
  } catch (e) { rows.push({ id: c.id, error: String(e.message).split('\n')[0] }); continue; }
  // nodes[0] is the root, nodes[1] the wrapping <div>, nodes[2] the first element under test.
  const n = json.nodes[2];
  if (!n) { rows.push({ id: c.id, error: 'no node' }); continue; }
  const t = n.bounds;
  const tess = { wrapperHeight: json.nodes[1].bounds.height, x: t.x, y: t.y, width: t.width, height: t.height, kind: n.kind };
  // An inline element's own box is its font's content area (ascent+descent), which no line-height
  // changes, and Tessera has no inline boxes at all, so for those the comparison is the wrapper's
  // height: the space the element takes in flow, which is what a layout feels.
  const inline = chrome.display === 'inline';
  // An inline-block control (button, input, textarea) sits on a line box, whose descender gap
  // grows the wrapper by a few px that no flex column reproduces: compare the control's own box.
  const keys = inline ? ['wrapperHeight']
    : chrome.display === 'inline-block' ? ['x', 'width', 'height']
    : ['wrapperHeight', 'x', 'y', 'width', 'height'];
  const d = keys.filter((k) => !(c.ignore || []).includes(k)).map((k) => Math.abs(chrome[k] - tess[k]));
  rows.push({ id: c.id, chrome: Object.fromEntries(Object.entries(chrome).map(([k, v]) => [k, typeof v === 'number' ? round(v) : v])),
              tessera: Object.fromEntries(Object.entries(tess).map(([k, v]) => [k, typeof v === 'number' ? round(v) : v])),
              ok: Math.max(...d) <= 0.5 });
}
await browser.close();

writeFileSync(join(out, 'results.json'), JSON.stringify(rows, null, 2));
const fmt = (o) => o ? `wrap ${round(o.wrapperHeight)} el ${o.x},${o.y} ${o.width}x${o.height}` : '-';
console.log('id'.padEnd(10), 'chrome'.padEnd(40), 'tessera'.padEnd(40), 'status');
for (const r of rows) {
  console.log(r.id.padEnd(10), r.error ? r.error : `${fmt(r.chrome).padEnd(40)} ${fmt(r.tessera).padEnd(40)} ${r.ok ? 'same' : 'DIFF'}`);
}
