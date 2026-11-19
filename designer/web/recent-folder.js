// SPDX-License-Identifier: MIT

// The recently opened project folders (issue #391, owner direction 2026-11-17): a bounded list of
// `FileSystemDirectoryHandle`s kept in IndexedDB, so the studio can list the projects a person opened
// before and reopen one with a click instead of the picker.
//
// A handle is a capability, not data: it survives a reload, but the browser drops its permission, so a
// stored handle is never read from here. `page-project-io.ts` asks `ensureWritable` on the click that
// reopens one, which is the user gesture a permission prompt needs. A zip has no handle to keep, so it is
// never listed. Every call degrades to "nothing remembered" when IndexedDB is unavailable (a private
// window, blocked site data): remembering a folder is a convenience and must never break opening one,
// and the list never replaces the files on disk.

/** At most this many projects are listed; the oldest falls off. */
export const MAX_RECENT = 10;

const DATABASE = 'tessera-studio';
const STORE = 'recent';
const LIST_KEY = 'folders';
const LEGACY_KEY = 'folder'; // the single remembered folder this list replaces

/**
 * Puts `handle` first in `list` (newest first), dropping an earlier entry for the same folder and
 * everything past `limit`. `same` decides whether two handles are one folder; the default is the platform's
 * `isSameEntry`, falling back to the name for a handle that has none (a test double).
 *
 * @param {Array<{name: string, openedAt: number, handle: any}>} list
 * @param {any} handle
 * @param {number} now
 * @param {(a: any, b: any) => Promise<boolean>} [same]
 */
export async function mergeRecent(list, handle, now, same = sameFolder, limit = MAX_RECENT) {
  const kept = [];
  for (const entry of list) {
    if (!(await same(entry.handle, handle))) {
      kept.push(entry);
    }
  }
  return [{ name: handle.name, openedAt: now, handle }, ...kept].slice(0, limit);
}

async function sameFolder(a, b) {
  try {
    if (typeof a.isSameEntry === 'function') {
      return await a.isSameEntry(b);
    }
  } catch {
    // fall through to the name
  }
  return a.name === b.name;
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

async function readList() {
  const list = await run('readonly', (store) => store.get(LIST_KEY));
  if (Array.isArray(list)) {
    return list.filter((entry) => entry && typeof entry.name === 'string' && entry.handle);
  }
  // A visitor from before the list existed has one handle under the old key: keep it as the first entry.
  const legacy = await run('readonly', (store) => store.get(LEGACY_KEY));
  return legacy && typeof legacy.name === 'string' ? [{ name: legacy.name, openedAt: 0, handle: legacy }] : [];
}

/** Remembers `handle` as the most recently opened folder. Never throws. */
export async function rememberFolder(handle) {
  try {
    const next = await mergeRecent(await readList(), handle, Date.now());
    await run('readwrite', (store) => store.put(next, LIST_KEY));
  } catch {
    // not remembered; opening the folder already worked
  }
}

/** The remembered folders, newest first: `[{ name, openedAt, handle }]`; empty when none or unavailable. */
export async function recallFolders() {
  try {
    return await readList();
  } catch {
    return [];
  }
}

/** Forgets one remembered folder (it no longer opens). Never throws. */
export async function forgetFolder(handle) {
  try {
    const list = await readList();
    const kept = [];
    for (const entry of list) {
      if (!(await sameFolder(entry.handle, handle))) {
        kept.push(entry);
      }
    }
    await run('readwrite', (store) => store.put(kept, LIST_KEY));
  } catch {
    // nothing to forget
  }
}

/** Clears the whole list ("Forget recent projects"). Never throws. */
export async function forgetAllFolders() {
  try {
    await run('readwrite', (store) => store.delete(LIST_KEY));
    await run('readwrite', (store) => store.delete(LEGACY_KEY));
  } catch {
    // nothing to forget
  }
}
