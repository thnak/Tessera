// SPDX-License-Identifier: MIT

// The last opened project folder (issue #391): a `FileSystemDirectoryHandle` kept in IndexedDB so the next
// visit can offer "Reopen <name>" instead of the picker.
//
// A handle is a capability, not data: it survives a reload, but the browser drops its permission, so the
// stored handle is never read from here. `page-project-io.ts` asks `ensureWritable` on the click that
// reopens it, which is the user gesture a permission prompt needs. Every call degrades to "nothing
// remembered" when IndexedDB is unavailable (a private window, blocked site data): remembering a folder
// is a convenience and must never break opening one.

const DATABASE = 'tessera-studio';
const STORE = 'recent';
const KEY = 'folder';

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

/** Remembers `handle` as the last opened folder. Never throws. */
export async function rememberFolder(handle) {
  try {
    await run('readwrite', (store) => store.put(handle, KEY));
  } catch {
    // not remembered; opening the folder already worked
  }
}

/** The remembered folder handle, or `null` when there is none or storage is unavailable. */
export async function recallFolder() {
  try {
    return (await run('readonly', (store) => store.get(KEY))) ?? null;
  } catch {
    return null;
  }
}

/** Forgets the remembered folder (it was removed, or the person declined it). Never throws. */
export async function forgetFolder() {
  try {
    await run('readwrite', (store) => store.delete(KEY));
  } catch {
    // nothing to forget
  }
}
