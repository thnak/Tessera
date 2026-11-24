// A helper for the browser gates: drive the studio's dock and its seeded project through the page.
//
// Two kinds of drift keep breaking these probes, and both are absorbed here rather than repeated in
// twelve scripts:
//
//   1. **Where a panel's tab is.** The dock redesign (issue #431, and the FlexLayout adapter since)
//      names a tab button `#flexlayout-tabbutton-<panel>`. It used to be `#tab-<panel>`, and every
//      probe that clicked the old id timed out. `tab()` is now the only place that knows the id, so the
//      next rename is a one-line fix here instead of an edit in nine files (issue #849).
//   2. **Whether a project is open.** The studio no longer boots with one (issue #391):
//      `page-boot.ts` returns `[]` unless a boot project is seeded, and a browser never seeds one. The
//      probes assumed `ui/App.tsx` and `styles/theme.css` were already in `#project-tree`, so
//      `selectFile` waited for a tree that never appeared. `ensureProject()` opens one through the
//      page's own Open Zip route when the tree is empty, which is what a visitor does.

export const ENTRY = 'ui/App.tsx';
export const SHEET = 'styles/theme.css';

/** The import that makes the entry's styles apply: the stylesheet is only used when it is imported. */
export const SHEET_IMPORT = 'import "../styles/theme.css";\n\n';

/** The dock panel ids the probes select. Listed so a typo is a name error, not a silent timeout. */
export const PANELS = { project: 'project', editor: 'editor', canvas: 'canvas', scene: 'scene' };

/** A minimal project the studio can open when the page booted without one. */
// The explicit `automationId` is what makes the tree addressable (automation-ids-v1.md): a node with
// no id reports `"0"` from `tessera_canvas_pick` and `tessera_canvas_style` refuses it, so a check that
// wants to ask "what did this node resolve to" needs one.
const SEED_ENTRY = `import "../styles/theme.css";

export function App() {
  return (
    <View automationId="1">
      <Text automationId="2">Probe</Text>
    </View>
  );
}
`;

const SEED_MANIFEST = `{
  "schemaVersion": 1,
  "app": { "name": "probe-project", "entry": "ui/App.tsx", "namespace": "probe_project" }
}
`;

const SEED_SHEET = `View {
  background: #101826;
}

Text {
  color: #e2e8f0;
  font-size: 24px;
}
`;

/**
 * The locator for a dock panel's tab button.
 *
 * One function, because the id is the dock library's to change: it is `flexlayout-tabbutton-<id>` today,
 * and the previous `#tab-<id>` is what broke every probe at once.
 */
export function tabButton(page, panel) {
  return page.locator(`#flexlayout-tabbutton-${panel}`);
}

/** Selects a dock panel by clicking its tab, then waits for the pane to be the visible one. */
export async function tab(page, panel) {
  const button = tabButton(page, panel);
  await button.click();
  // A tab click is synchronous in the dock's model but the pane's content may mount a frame later;
  // waiting for the tab to be selected is the cheap, deterministic signal that the switch happened.
  await page
    .locator(`#flexlayout-tabbutton-${panel}[aria-selected="true"]`)
    .waitFor({ state: 'attached', timeout: 5000 });
  await page.waitForTimeout(150);
}

/**
 * Opens a workbench panel that is not in the default layout (issue #849).
 *
 * `diagnostics` is a real panel, but the default layout keeps its group (`g-bottom`) collapsed, so it
 * has **no tab button** until it is activated - a probe that clicked `#tab-diagnostics` was waiting
 * for an element the layout never renders. Activating through the workbench store is what a person
 * does with the Command Palette's "Show panel area", and it makes the tab appear afterwards.
 *
 * Returns `true` when the panel is visible.
 */
export async function openPanel(page, panelId) {
  const selector = `#flexlayout-tabbutton-${panelId}`;
  // Idempotent: the panel area is a toggle, so calling this on an already-open panel would collapse it
  // again. A probe calls it more than once (once before the first read, once before a later one), so
  // "already there" has to be a no-op.
  if ((await page.locator(selector).count()) > 0) {
    return true;
  }
  const ran = await page.evaluate(() => {
    const workbench = globalThis.__tesseraWorkbench;
    if (!workbench || typeof workbench.run !== "function") {
      return false;
    }
    workbench.run("view.togglePanelArea");
    return true;
  });
  if (!ran) {
    return false;
  }
  return page
    .locator(selector)
    .waitFor({ state: 'attached', timeout: 5000 })
    .then(() => true)
    .catch(() => false);
}

/** Whether the page currently has a project open (the tree is populated). */
export async function hasProject(page) {
  return page.evaluate(() => document.querySelector('#project-tree [role="treeitem"]') !== null);
}

/**
 * Makes sure a project is open, opening the probe's seed through the page's Open Zip route if not.
 *
 * Returns `true` when a project is open afterwards. A page that shipped without the file input cannot
 * be helped from here, so the caller sees `false` and can report it rather than hang on a click.
 */
export async function ensureProject(page) {
  if (await hasProject(page)) {
    return true;
  }
  const input = page.locator('input[type="file"][accept*="zip"]');
  if ((await input.count()) === 0) {
    return false;
  }
  // The zip is built in the page's own writer so the bytes match what Save produces (issue #849's
  // "open a project through the page's Open Zip route"): a hand-rolled archive would test a different
  // reader path than the studio's own.
  const buffer = await buildSeedZip();
  // Opening a zip over an open project asks "replace?"; accepting is what the probes want either way.
  page.on('dialog', (dialog) => void dialog.accept());
  await input.setInputFiles({ name: 'probe-project.zip', mimeType: 'application/zip', buffer });
  await page
    .locator('#project-tree [role="treeitem"]')
    .first()
    .waitFor({ state: 'attached', timeout: 20000 });
  return true;
}

/** Builds the seed project's zip with the studio's own writer, so the format cannot drift here. */
async function buildSeedZip() {
  const { writeZip } = await import('../../designer/web/studio-zip.js');
  const encoder = new TextEncoder();
  const files = [
    ['tessera.json', SEED_MANIFEST],
    [ENTRY, SEED_ENTRY],
    [SHEET, SEED_SHEET],
  ];
  // `writeZip` takes bytes and sorts by path itself, so the archive is deterministic.
  return Buffer.from(writeZip(files.map(([path, text]) => ({ path, bytes: encoder.encode(text) }))));
}

/**
 * The computed value the engine resolved for one property of one node (issue #849).
 *
 * **Why not a screenshot.** Headless Chromium does not composite a presented WebGPU frame, so
 * `page.locator('#canvas').screenshot()` comes back black even while the pane reports
 * `drew N draw call(s)` - confirmed on a host where the same page draws. A probe that read the
 * canvas by colour was therefore asserting against `rgb(0, 0, 0)` and could never pass on CI.
 * `tessera_canvas_style` answers the same question without the compositor: it is what the engine
 * resolved and painted *with*, not what the compositor happened to hand back.
 *
 * `node` is the `automationId` the scene declares (automation-ids-v1.md). Returns `null` when no
 * device exists yet or the node is unknown, so a caller reports a verdict instead of crashing.
 */
export async function computedStyle(page, node, property) {
  const report = await page.evaluate(
    ([id]) => {
      const canvas = globalThis.__tesseraCanvas;
      if (!canvas || typeof canvas.styleInfo !== "function") {
        return null;
      }
      return canvas.styleInfo(id);
    },
    [String(node)],
  );
  if (report === null || report.ok !== true) {
    return null;
  }
  const computed = report.computed ?? {};
  return typeof computed[property] === "string" ? computed[property] : null;
}

/**
 * `#rrggbb` (or `rgb(...)`) to `{ r, g, b }`, so a probe can compare channels instead of strings.
 * Returns `null` for a value it does not understand, which a caller reports rather than guessing at.
 */
export function rgbOf(value) {
  if (typeof value !== "string") {
    return null;
  }
  const hex = /^#([0-9a-f]{6})$/i.exec(value.trim());
  if (hex !== null) {
    const n = Number.parseInt(hex[1], 16);
    return { r: (n >> 16) & 0xff, g: (n >> 8) & 0xff, b: n & 0xff };
  }
  const rgb = /^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/i.exec(value.trim());
  if (rgb !== null) {
    return { r: Number(rgb[1]), g: Number(rgb[2]), b: Number(rgb[3]) };
  }
  return null;
}

/** Selects `path` in the project tree, so the editor shows it. */
export async function selectFile(page, path) {
  if (!(await ensureProject(page))) {
    throw new Error(`selectFile: no project is open and the page has no Open Zip route for ${path}`);
  }
  await page.locator(`#project-tree [role="treeitem"][data-path="${path}"]`).click();
  await page.waitForTimeout(150);
}

/** Selects `path` in the project tree and replaces its text with `text`. */
export async function editFile(page, path, text) {
  await selectFile(page, path);
  await page.evaluate((next) => window.__tesseraEditor.edit(next), text);
}

/** Replaces the entry source; the stylesheet import is prepended so the sheet still applies. */
export async function editEntry(page, source) {
  await editFile(page, ENTRY, SHEET_IMPORT + source);
}

/** Replaces the stylesheet's text. */
export async function editSheet(page, css) {
  await editFile(page, SHEET, css);
}
