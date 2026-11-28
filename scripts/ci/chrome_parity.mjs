#!/usr/bin/env node
/**
 * Render the parity form in Chrome so it can be compared with what the engine paints.
 *
 * **What this is.** `samples/gallery` (`--page form`) paints `FormPage.tsx` with the engine;
 * `tools/parity/form.html` is the same structure as HTML. Both read the same stylesheets
 * (`packages/tessera-ui/button.css`, `samples/gallery/form.module.css`), the same theme tokens
 * (`instrument.theme.json`, injected here as `:root` variables) and the same font
 * (Plex Sans Regular from the font pack, `packages/tessera-fonts/ibm-plex`, #970). This script is the browser half: it writes
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
 *                                     [--module-css <dir>]
 *
 * The form's stylesheet is a CSS module (issue #463): its class names are scoped at compile time.
 * Run `gallery --emit-module-css <dir>` first; it writes `form.scoped.css` and `form.classes.json`
 * with the scan's own output, and `--module-css` (default: `--out`) points here at them, so the
 * browser twin uses exactly the names and rules the engine did.
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
// `--click CLASS[:N]` clicks the Nth element with that class, and `--advance-ms` then freezes every
// running transition and animation at that many milliseconds, the instant the engine's
// `gallery --click ... --advance-ms` captures.
const clickSpec = argument('--click', null);
const advanceMs = Number(argument('--advance-ms', '0'));

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
const fontFile = (name) => pathToFileURL(join(root, `packages/tessera-fonts/ibm-plex/fonts/${name}.ttf`)).href;
// The theme names IBM Plex and the engine is handed the pack's Plex Sans Regular, SemiBold and Bold
// (parity_gate.py, #973) and picks one per run by weight, so the browser declares the same three
// files at the weights they carry: neither side synthesizes a bold.
const fontFaces = [
  [400, 'IBMPlexSans-Regular'],
  [600, 'IBMPlexSans-SemiBold'],
  [700, 'IBMPlexSans-Bold'],
]
  .map(([weight, name]) => `@font-face { font-family: "ParityPlex"; font-weight: ${weight}; src: url("${fontFile(name)}"); }\n`)
  .join('');
const tokenCss =
  fontFaces +
  `:root {\n${Object.entries(tokens).map(([k, v]) => `  ${k}: ${v};`).join('\n')}\n  --font-sans: "ParityPlex", sans-serif;\n}\n` +
  `html, body { background: ${tokens['--window-background']}; color: ${tokens['--window-foreground']}; font-family: "ParityPlex", sans-serif; }\n`;

await mkdir(out, { recursive: true });
const browser = await chromium.launch(chromePath ? { executablePath: chromePath } : {});
const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: dpr });
let classMap = {};
let pageUrl = pathToFileURL(join(root, `tools/parity/${pageName}.html`)).href;
if (pageName === 'form') {
  // The twin is authored with the module's local class names; write a scoped copy beside the other
  // outputs (links made absolute) and open that.
  const moduleDir = resolve(argument('--module-css', out));
  let scopedCss;
  try {
    scopedCss = await readFile(join(moduleDir, 'form.scoped.css'), 'utf8');
    classMap = JSON.parse(await readFile(join(moduleDir, 'form.classes.json'), 'utf8'));
  } catch {
    console.error(`no form.scoped.css / form.classes.json in ${moduleDir}: run gallery --emit-module-css <dir> first`);
    process.exit(2);
  }
  const repoUrl = pathToFileURL(root).href;
  const scoped = (await readFile(join(root, 'tools/parity/form.html'), 'utf8'))
    .replace(/href="\.\.\/\.\.\//g, `href="${repoUrl}/`)
    .replace(/class="([^"]*)"/g, (_, names) =>
      `class="${names.split(/\s+/).filter(Boolean).map((name) => classMap[name] ?? name).join(' ')}"`)
    .replace('</head>', `<style>
${scopedCss}
</style>
<script>window.__classMap = ${JSON.stringify(classMap)};</script>
</head>`);
  const scopedPath = join(out, 'form.scoped.html');
  await writeFile(scopedPath, scoped);
  pageUrl = pathToFileURL(scopedPath).href;
}
await page.goto(pageUrl);
await page.addStyleTag({ content: tokenCss });
await page.evaluate(() => document.fonts.ready);
await page.waitForTimeout(150);
if (clickSpec) {
  const [authored, index = '0'] = clickSpec.split(':');
  // The click names an authored class; the page carries the scoped one when the page is a module.
  const cls = classMap[authored] ?? authored;
  await page.locator(`.${cls}`).nth(Number(index)).click({ force: true });
  await page.evaluate(
    (ms) => {
      document.body.offsetHeight; // flush style so the transitions exist
      for (const animation of document.getAnimations()) {
        animation.pause();
        animation.currentTime = ms;
      }
    },
    advanceMs,
  );
}
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
