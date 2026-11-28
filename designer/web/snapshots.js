// SPDX-License-Identifier: MIT

// Version history for project files (issue #583): local snapshots, the pure half.
//
// A snapshot is the text of one file at the moment of a save (or just before a format or rename). The
// history lives in this browser only -- nothing is uploaded and nothing enters a share link -- and is a
// convenience, never the file: restoring one is an ordinary edit (`restoreEdit`) the editor can undo.
//
// The rules, all deterministic (no clock, no random ids; the caller passes `at`):
//  * **Bounded**: at most `maxPerFile` snapshots per path and `maxTotalBytes` of text per project; the oldest
//    go first. A file over `maxFileBytes` is refused with a named reason, never truncated.
//  * **Deduplicated**: a save whose text equals the file's newest snapshot adds nothing.
//  * **Deterministic ids**: `<path hash>-<content hash>-<sequence>`, so the same history replays to the same ids.
//  * **Exact**: `restoreEdit(current, snapshotText)` is one replacement that yields the snapshot's bytes.
//
// `snapshot-store.js` is the storage half; this module has no browser dependency and is tested in Node.

/** Bounds of one project's history; the same scale as a draft. */
export const SNAPSHOT_LIMITS = Object.freeze({ maxPerFile: 50, maxFileBytes: 1 << 20, maxTotalBytes: 20 << 20 });

/** The reasons a snapshot is recorded; carried so the timeline can say why a version exists. */
export const SNAPSHOT_REASONS = Object.freeze(['save', 'format', 'rename', 'restore']);

/** @typedef {{id: string, path: string, reason: string, at: number, bytes: number, text: string}} Snapshot */
/** @typedef {{version: 1, next: number, entries: Snapshot[]}} History (entries oldest first) */

/** @returns {History} */
export function emptyHistory() {
  return { version: 1, next: 1, entries: [] };
}

/** FNV-1a over UTF-16 code units, 32 bit, as 8 hex digits. */
export function hash32(text, seed = 0x811c9dc5) {
  let hash = seed >>> 0;
  for (let index = 0; index < text.length; index += 1) {
    hash = Math.imul(hash ^ text.charCodeAt(index), 0x01000193) >>> 0;
  }
  return hash.toString(16).padStart(8, '0');
}

/** UTF-8 size of `text` without allocating it. */
export function utf8Bytes(text) {
  let bytes = 0;
  for (let index = 0; index < text.length; index += 1) {
    const unit = text.charCodeAt(index);
    if (unit < 0x80) {
      bytes += 1;
    } else if (unit < 0x800) {
      bytes += 2;
    } else if (unit >= 0xd800 && unit < 0xdc00 && index + 1 < text.length) {
      bytes += 4; // a surrogate pair is one 4-byte code point
      index += 1;
    } else {
      bytes += 3;
    }
  }
  return bytes;
}

/** Drops malformed entries from a stored value: a corrupt history is an empty one, never an error. */
export function sanitizeHistory(value) {
  if (!value || value.version !== 1 || !Array.isArray(value.entries) || !Number.isSafeInteger(value.next)) {
    return emptyHistory();
  }
  const entries = value.entries.filter(
    (entry) => entry && typeof entry.id === 'string' && typeof entry.path === 'string' && typeof entry.text === 'string',
  );
  return { version: 1, next: value.next, entries };
}

/**
 * Records `text` of `path`.
 *
 * @param {History} history not modified; a new history is returned
 * @param {{path: string, text: string, reason: string, at: number}} input
 * @param {typeof SNAPSHOT_LIMITS} [limits]
 * @returns {{history: History, added: boolean, evicted: number, skipped?: 'duplicate' | 'too-large'}}
 */
export function addSnapshot(history, input, limits = SNAPSHOT_LIMITS) {
  const { path, text, reason, at } = input;
  const bytes = utf8Bytes(text);
  if (bytes > limits.maxFileBytes) {
    return { history, added: false, evicted: 0, skipped: 'too-large' };
  }
  let newest;
  for (const entry of history.entries) {
    if (entry.path === path) {
      newest = entry;
    }
  }
  if (newest !== undefined && newest.text === text) {
    return { history, added: false, evicted: 0, skipped: 'duplicate' };
  }
  const id = `${hash32(path)}-${hash32(text)}-${history.next}`;
  let entries = [...history.entries, { id, path, reason, at, bytes, text }];
  const before = entries.length;

  let ofPath = entries.filter((entry) => entry.path === path).length;
  entries = entries.filter((entry) => {
    if (entry.path === path && ofPath > limits.maxPerFile) {
      ofPath -= 1;
      return false;
    }
    return true;
  });
  let total = entries.reduce((sum, entry) => sum + entry.bytes, 0);
  // The new snapshot is last and always fits on its own (maxFileBytes <= maxTotalBytes), so the loop ends.
  while (total > limits.maxTotalBytes && entries.length > 1) {
    total -= entries[0].bytes;
    entries = entries.slice(1);
  }
  return { history: { version: 1, next: history.next + 1, entries }, added: true, evicted: before - entries.length };
}

/** The timeline of one file, newest first. */
export function timeline(history, path) {
  return history.entries.filter((entry) => entry.path === path).reverse();
}

/** The snapshot with `id`, or `undefined`. */
export function findSnapshot(history, id) {
  return history.entries.find((entry) => entry.id === id);
}

/** Moves the history of a renamed file with it. */
export function renamePath(history, from, to) {
  return { ...history, entries: history.entries.map((entry) => (entry.path === from ? { ...entry, path: to } : entry)) };
}

/** Moves the history of a renamed folder (every path under `from/`) with it; `from` and `to` have no trailing slash. */
export function renameTree(history, from, to) {
  const prefix = `${from}/`;
  return {
    ...history,
    entries: history.entries.map((entry) => (entry.path.startsWith(prefix) ? { ...entry, path: `${to}/${entry.path.slice(prefix.length)}` } : entry)),
  };
}

/** The history without `path` (the file was deleted and the person chose to drop its versions). */
export function forgetPath(history, path) {
  return { ...history, entries: history.entries.filter((entry) => entry.path !== path) };
}

/**
 * The one replacement that turns `current` into `target` (UTF-16 offsets into `current`), found by trimming the
 * common prefix and suffix. The editor dispatches it as a normal transaction, so Undo brings the working text
 * back. Surrogate pairs are never split.
 *
 * @returns {{from: number, to: number, insert: string}}
 */
export function restoreEdit(current, target) {
  let start = 0;
  const limit = Math.min(current.length, target.length);
  while (start < limit && current.charCodeAt(start) === target.charCodeAt(start)) {
    start += 1;
  }
  let endCurrent = current.length;
  let endTarget = target.length;
  while (endCurrent > start && endTarget > start && current.charCodeAt(endCurrent - 1) === target.charCodeAt(endTarget - 1)) {
    endCurrent -= 1;
    endTarget -= 1;
  }
  // Do not cut inside a surrogate pair: widen the edit to include the whole pair.
  const isHigh = (unit) => unit >= 0xd800 && unit < 0xdc00;
  const isLow = (unit) => unit >= 0xdc00 && unit < 0xe000;
  if (start > 0 && isHigh(current.charCodeAt(start - 1)) && isLow(current.charCodeAt(start) || 0)) {
    start -= 1;
  }
  if (endCurrent < current.length && isHigh(current.charCodeAt(endCurrent - 1)) && isLow(current.charCodeAt(endCurrent))) {
    endCurrent += 1;
    endTarget += 1;
  }
  return { from: start, to: endCurrent, insert: target.slice(start, endTarget) };
}

/** Applies `restoreEdit`'s result to `current`. */
export function applyEdit(current, edit) {
  return current.slice(0, edit.from) + edit.insert + current.slice(edit.to);
}
