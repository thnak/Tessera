#!/usr/bin/env node
/**
 * Render the parity form in Chrome so it can be compared with what the engine paints.
 *
 * **What this is.** `samples/gallery` (`--page form`) paints `FormPage.tsx` with the engine;
 * `tools/parity/form.html` is the same structure as HTML. Both read the same stylesheets
 * (`packages/tessera-ui/button.css`, `samples/gallery/form.css`), the same theme tokens
 * (`instrument.theme.json`, injected here as `:root` variables) and the same font
 * (`tests/fixtures/fonts/NotoSans-Regular.ttf`). This script is the browser half: it writes
 * `chrome.png` and `chrome-boxes.json` (the border box of every classed element, in CSS pixels).
 * `scripts/ci/chrome_parity_compare.py` is the other half: it diffs `chrome.png` against the
 * engine's capture and reports the result.
 *
 * Developer-run, not a CI gate: it needs a browser, and the pixel difference between a browser and
 * the engine's text rasterizer is never zero, so there is no threshold to gate on yet.
 *
 * Usage (playwright is not a repository dependency):
 *   node scripts/ci/chrome_parity.mjs --out out/parity [--width 640] [--height 720] [--theme instrument-light]
 *                                     [--chrome <path to chrome>] [--dpr 1] [--repo <repository root>]
 */

import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

function argument(name, fallback) {
  const index = process.argv.indexOf(name);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
}

const root = resolve(argument('--repo', join(dirname(fileURLToPath(import.meta.url)), '..', '..')));
const out = resolve(argument('--out', 'out/parity'));
const width = Number(argument('--width', '640'));
const height = Number(argument('--height', '720'));
const dpr = Number(argument('--dpr', '1'));
const themeName = argument('--theme', 'instrument-light');
const chromePath = argument('--chrome', null);
const pageName = argument('--page', 'form');

let chromium;
try {
  ({ chromium } = await import('playwright'));
} catch {
  console.error('playwright is not installed: npm install playwright (and point --chrome at a Chrome if no download)');
  process.exit(2);
}

const theme = JSON.parse(await readFile(join(root, 'packages/tessera-ui/themes/instrument.theme.json'), 'utf8'));
const tokens = theme.themes[themeName];
if (!tokens) {
  console.error(`unknown theme ${themeName}`);
  process.exit(2);
}
const fontUrl = pathToFileURL(join(root, 'tests/fixtures/fonts/NotoSans-Regular.ttf')).href;
// The theme names IBM Plex; the engine is handed Noto Sans, so the browser is too.
const tokenCss =
  `@font-face { font-family: "ParityNoto"; src: url("${fontUrl}"); }\n` +
  `:root {\n${Object.entries(tokens).map(([k, v]) => `  ${k}: ${v};`).join('\n')}\n  --font-sans: "ParityNoto", sans-serif;\n}\n` +
  `html, body { background: ${tokens['--window-background']}; color: ${tokens['--window-foreground']}; font-family: "ParityNoto", sans-serif; }\n`;

await mkdir(out, { recursive: true });
const browser = await chromium.launch(chromePath ? { executablePath: chromePath } : {});
const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: dpr });
await page.goto(pathToFileURL(join(root, `tools/parity/${pageName}.html`)).href);
await page.addStyleTag({ content: tokenCss });
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(150);
await page.screenshot({ path: join(out, 'chrome.png') });
const boxes = await page.evaluate(() =>
  [...document.querySelectorAll('[class]')].map((el) => {
    const r = el.getBoundingClientRect();
    return { class: el.getAttribute('class'), x: r.x, y: r.y, w: r.width, h: r.height };
  }),
);
await writeFile(join(out, 'chrome-boxes.json'), JSON.stringify(boxes, null, 1));
await browser.close();
console.log(`chrome_parity: wrote ${join(out, 'chrome.png')} (${width}x${height} @${dpr}) and ${boxes.length} boxes`);
