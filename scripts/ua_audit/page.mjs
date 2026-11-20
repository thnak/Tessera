#!/usr/bin/env node
// SPDX-License-Identifier: MIT
/**
 * Page parity (issue #607): a sample's browser twin and its native render, compared element by
 * element in document order, at several viewport widths. The twin has no reset (ADR-063), so a
 * difference is either a gap to fix or a named exception (docs/reference/ua-audit.md).
 *
 *   node scripts/ua_audit/page.mjs --chrome <chrome> --preview <tessera_scene_preview> \
 *        --twin tools/parity/showcase.html --tsx samples/showcase/Showcase.tsx --component Showcase \
 *        --css samples/showcase/app.css,samples/showcase/showcase.module.css [--sizes 900x700,400x800]
 *
 * The TSX is rewritten for the preview CLI (one stylesheet, `styles.x` -> `"x"`). Tessera's preview
 * measures text with a stub, so a text node that wraps differs by line count; those rows are
 * reported as `text` and are not counted as geometry differences.
 */
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

const arg = (n, d) => { const i = process.argv.indexOf(n); return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : d; };
const preview = resolve(arg('--preview', ''));
const twin = resolve(arg('--twin', ''));
const tsxPath = arg('--tsx', '');
const component = arg('--component', '');
const css = arg('--css', '').split(',').filter(Boolean);
const sizes = arg('--sizes', '900x700,400x800').split(',');
const out = resolve(arg('--out', 'out/page_parity'));
const tol = Number(arg('--tol', '1'));
mkdirSync(join(out, 'ui'), { recursive: true });

const { chromium } = createRequire(import.meta.url)('playwright');
const browser = await chromium.launch(arg('--chrome', null) ? { executablePath: arg('--chrome') } : {});

writeFileSync(join(out, 'ui', 'Preview.css'), css.map((f) => readFileSync(f, 'utf8')).join('\n'));
writeFileSync(join(out, 'ui', 'Preview.tsx'), readFileSync(tsxPath, 'utf8')
  .replace(/^import "\.\/[^"]+\.css";/m, 'import "./Preview.css";')
  .replace(/^import styles from .*$/m, '')
  .replace(/styles\.([A-Za-z]+)/g, '"$1"'));

let bad = 0;
for (const size of sizes) {
  const [w, h] = size.split('x').map(Number);
  const page = await browser.newPage({ viewport: { width: w, height: h } });
  await page.goto(pathToFileURL(twin).href);
  await page.evaluate(() => document.fonts.ready);
  const chrome = await page.evaluate(() => {
    const root = document.querySelector('.app');
    return [root, ...root.querySelectorAll('*')].map((e) => {
      const r = e.getBoundingClientRect();
      const lines = e.children.length === 0 ? Math.round(r.height / parseFloat(getComputedStyle(e).lineHeight || '1')) : 0;
      return { tag: e.tagName.toLowerCase(), cls: e.className, x: r.x, y: r.y, width: r.width, height: r.height, leaf: e.children.length === 0 };
    });
  });
  await page.close();
  const json = JSON.parse(execFileSync(preview, ['--scene-json', '-', join(out, 'ui', 'Preview.tsx'), '--component', component,
    '--size', size, '--stylesheet', join(out, 'ui', 'Preview.css'), '--file', 'ui/Preview.tsx'], { encoding: 'utf8', maxBuffer: 1 << 26 }));
  // A button's caption is a Text child natively and a bare text node in the browser: skip it.
  const nodes = json.nodes.slice(1).filter((n) => json.nodes[n.parent]?.kind !== 'Button');
  console.log(`\n== ${size}: ${chrome.length} browser elements, ${nodes.length} native nodes`);
  if (chrome.length !== nodes.length) { console.log('structure differs: cannot align'); bad++; continue; }
  chrome.forEach((c, i) => {
    const t = nodes[i].bounds;
    const d = ['x', 'y', 'width', 'height'].map((k) => Math.abs(c[k] - t[k]));
    const worst = Math.max(...d);
    const textOnly = c.leaf && worst > tol && Math.abs(c.x - t.x) <= tol && Math.abs(c.y - t.y) <= tol;
    if (worst > tol) {
      if (!textOnly) bad++;
      console.log(`${textOnly ? 'text' : 'DIFF'} #${i} <${c.tag} .${c.cls}> chrome ${c.x.toFixed(1)},${c.y.toFixed(1)} ${c.width.toFixed(1)}x${c.height.toFixed(1)}  native ${t.x.toFixed(1)},${t.y.toFixed(1)} ${t.width.toFixed(1)}x${t.height.toFixed(1)}`);
    }
  });
}
await browser.close();
console.log(bad === 0 ? '\nall rows match' : `\n${bad} difference(s)`);
