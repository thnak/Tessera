// A helper for the browser gates: edit a file of the studio's seeded project through the page.
//
// The studio previews a project (its entry from `tessera.json`, the files that entry imports), so a gate
// that wants a different source or a different stylesheet edits that *project file*: it selects the file in
// the tree, then types into the editor, exactly as a visitor does.

export const ENTRY = 'ui/App.tsx';
export const SHEET = 'styles/theme.css';

/** The import that makes the entry's styles apply: the stylesheet is only used when it is imported. */
export const SHEET_IMPORT = 'import "../styles/theme.css";\n\n';

/** Selects `path` in the project tree, so the editor shows it. */
export async function selectFile(page, path) {
  await page.locator(`#project-tree [role="treeitem"][data-path="${path}"]`).click();
  await page.waitForTimeout(150);
}

/** Selects `path` in the project tree and replaces its text with `text`. */
export async function editFile(page, path, text) {
  await selectFile(page, path);
  await page.locator('#source').fill(text);
}

/** Replaces the entry source; the stylesheet import is prepended so the sheet still applies. */
export async function editEntry(page, source) {
  await editFile(page, ENTRY, SHEET_IMPORT + source);
}

/** Replaces the stylesheet's text. */
export async function editSheet(page, css) {
  await editFile(page, SHEET, css);
}
