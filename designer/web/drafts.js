// SPDX-License-Identifier: MIT

// Unsaved drafts (issue #391): the edits a person has not Saved, kept in this browser so a reload or a
// crashed tab does not lose them.
//
// **A draft is a convenience, never the file.** It lives in IndexedDB of this origin only, it is offered
// back -- never applied silently -- and the files on disk stay the truth. The rule that keeps it honest is
// `applicableDraft`: every file in a draft records the text it was edited **from** (`base`), and a draft
// entry is restored only while the project that was just opened still holds exactly that text. A file that
// changed on disk since (or a zip that is another version) is skipped and counted, so an old draft can
// never overwrite a newer file.
//
// The pure half (`buildDraft`, `applicableDraft`, `projectKey`) has no browser dependency and is tested in
// Node; the storage half degrades to "no draft" whenever IndexedDB is unavailable (a private window,
// blocked site data), and never throws into the page.

const DATABASE = 'tessera-studio-drafts';
const STORE = 'drafts';

/** Bounds a draft the same way a project is bounded: unsaved text is accepted text. */
export const DRAFT_LIMITS = Object.freeze({ maxFiles: 500, maxFileBytes: 1 << 20, maxTotalBytes: 20 << 20 });

/** The storage key of a project: where it came from and what it is called. */
export function projectKey(source, name) {
  return `${source}:${name}`;
}

/**
 * The draft for the current edits, or `null` when nothing is unsaved.
 *
 * @typedef {{path: string, text: string, base: string | null}} DraftEdit
 * @typedef {{path: string, base: string | null}} DraftRemoval
 * @typedef {{savedAt: number, edits: DraftEdit[], removed: DraftRemoval[]}} Draft
 *
 * @param {readonly {path: string, text: string}[]} files the project's files as opened or last saved
 * @param {(path: string) => string} textOf the editor's current text of a file
 * @param {{dirty: readonly string[], added: readonly string[], removed: readonly string[], removedBase?: ReadonlyMap<string, string>}} marks
 * @returns {Draft | null}
 */
export function buildDraft(files, textOf, marks) {
  const baseOf = new Map(files.map((file) => [file.path, file.text]));
  const edits = [];
  let total = 0;
  for (const path of marks.dirty) {
    const text = textOf(path);
    total += text.length;
    edits.push({
      path,
      text,
      // A file created in the page has no earlier text; `null` means "restore only if the path is free".
      base: marks.added.includes(path) ? null : (baseOf.get(path) ?? null),
    });
  }
  // A deleted file is gone from `files`; its base text is the project's copy the caller passes in `removedBase`.
  const removed = marks.removed.map((path) => ({ path, base: marks.removedBase?.get(path) ?? null }));
  if (edits.length === 0 && removed.length === 0) {
    return null;
  }
  if (edits.length > DRAFT_LIMITS.maxFiles || total > DRAFT_LIMITS.maxTotalBytes) {
    return null; // too large to keep; the person still has the page and Download
  }
  if (edits.some((edit) => edit.text.length > DRAFT_LIMITS.maxFileBytes)) {
    return null;
  }
  return { savedAt: Date.now(), edits, removed };
}

/**
 * Splits a draft against the project just opened into what can be restored and what cannot.
 *
 * @param {Draft} draft
 * @param {readonly {path: string, text: string}[]} files
 * @returns {{edits: DraftEdit[], removed: string[], skipped: string[]}}
 */
export function applicableDraft(draft, files) {
  const current = new Map(files.map((file) => [file.path, file.text]));
  const edits = [];
  const removed = [];
  const skipped = [];
  for (const edit of draft.edits) {
    const here = current.get(edit.path);
    const fits = edit.base === null ? here === undefined : here === edit.base;
    (fits ? edits : skipped).push(fits ? edit : edit.path);
  }
  for (const gone of draft.removed) {
    const here = current.get(gone.path);
    if (gone.base !== null && here === gone.base) {
      removed.push(gone.path);
    } else {
      skipped.push(gone.path);
    }
  }
  return { edits, removed, skipped };
}

function open() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE, 1);
    request.onupgradeneeded = () => request.result.createObjectStore(STORE);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

async function run(mode, action) {
  const database = await open();
  try {
    return await new Promise((resolve, reject) => {
      const request = action(database.transaction(STORE, mode).objectStore(STORE));
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  } finally {
    database.close();
  }
}

/** Stores the draft for `key`. Never throws. */
export async function saveDraft(key, draft) {
  try {
    await run('readwrite', (store) => store.put(draft, key));
  } catch {
    // not kept; the edits are still on the page
  }
}

/** The stored draft for `key`, or `null`. Never throws. */
export async function loadDraft(key) {
  try {
    return (await run('readonly', (store) => store.get(key))) ?? null;
  } catch {
    return null;
  }
}

/** Removes the draft for `key` (saved, restored or discarded). Never throws. */
export async function clearDraft(key) {
  try {
    await run('readwrite', (store) => store.delete(key));
  } catch {
    // nothing to remove
  }
}
