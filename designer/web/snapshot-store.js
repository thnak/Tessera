// SPDX-License-Identifier: MIT

// Version history storage (issue #583): the browser half.
//
// One value per project key in this origin's IndexedDB (`tessera-studio-history`), the whole `History` of
// `snapshots.js`. It follows the contract of `drafts.js`: storage that is missing or blocked means "no
// history", never an exception into the page, and a full disk is a **named** result (`quota`) the page can
// show, not a silent loss. The key-value layer is injected so Node can drive it with a fake that throws.

import { SNAPSHOT_LIMITS, addSnapshot, emptyHistory, sanitizeHistory } from './snapshots.js';

const DATABASE = 'tessera-studio-history';
const STORE = 'history';

/** A key-value store over IndexedDB: `get(key)`, `put(key, value)`, `delete(key)`; each may reject. */
export function indexedDbKeyValue(factory = globalThis.indexedDB) {
  const open = () =>
    new Promise((resolve, reject) => {
      if (!factory) {
        reject(new Error('indexedDB unavailable'));
        return;
      }
      const request = factory.open(DATABASE, 1);
      request.onupgradeneeded = () => request.result.createObjectStore(STORE);
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
  const run = async (mode, action) => {
    const database = await open();
    try {
      return await new Promise((resolve, reject) => {
        const transaction = database.transaction(STORE, mode);
        const request = action(transaction.objectStore(STORE));
        transaction.oncomplete = () => resolve(request.result);
        transaction.onabort = () => reject(transaction.error);
        request.onerror = () => reject(request.error);
      });
    } finally {
      database.close();
    }
  };
  return {
    get: (key) => run('readonly', (store) => store.get(key)),
    put: (key, value) => run('readwrite', (store) => store.put(value, key)),
    delete: (key) => run('readwrite', (store) => store.delete(key)),
  };
}

/** True for the error a full origin quota raises (name differs by browser). */
export function isQuotaError(error) {
  return Boolean(error) && (error.name === 'QuotaExceededError' || error.code === 22 || error.name === 'NS_ERROR_DOM_QUOTA_REACHED');
}

/**
 * @param {{get: Function, put: Function, delete: Function}} kv
 */
export function createSnapshotStore(kv) {
  return {
    /** The stored history for `key`; an empty one when absent, corrupt or storage fails. Never throws. */
    async load(key) {
      try {
        return sanitizeHistory(await kv.get(key));
      } catch {
        return emptyHistory();
      }
    },

    /** Stores `history`. Resolves `{ok: true}` or `{ok: false, reason: 'quota' | 'unavailable'}`. Never throws. */
    async save(key, history) {
      try {
        await kv.put(key, history);
        return { ok: true };
      } catch (error) {
        return { ok: false, reason: isQuotaError(error) ? 'quota' : 'unavailable' };
      }
    },

    /**
     * Loads, adds one snapshot and stores. A quota failure is retried once after halving the byte budget
     * (so the oldest versions give way); if that fails too the result says `quota` and the stored history is
     * unchanged.
     *
     * @returns {Promise<{ok: boolean, added: boolean, reason?: string, skipped?: string, history: object}>}
     */
    async record(key, input, limits = SNAPSHOT_LIMITS) {
      const loaded = await this.load(key);
      const first = addSnapshot(loaded, input, limits);
      if (!first.added) {
        return { ok: true, added: false, skipped: first.skipped, history: loaded };
      }
      let saved = await this.save(key, first.history);
      if (saved.ok) {
        return { ok: true, added: true, history: first.history };
      }
      if (saved.reason === 'quota') {
        const smaller = addSnapshot(loaded, input, { ...limits, maxTotalBytes: Math.max(limits.maxFileBytes, limits.maxTotalBytes >> 1) });
        saved = await this.save(key, smaller.history);
        if (saved.ok) {
          return { ok: true, added: true, history: smaller.history };
        }
      }
      return { ok: false, added: false, reason: saved.reason, history: loaded };
    },

    /** Drops the whole history of `key`. Never throws. */
    async clear(key) {
      try {
        await kv.delete(key);
      } catch {
        // nothing to remove
      }
    },
  };
}
