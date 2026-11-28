// SPDX-License-Identifier: MIT

// A deterministic line diff for the studio's version history (issue #583).
//
// Lines keep their terminator, so every segment carries UTF-16 offsets (`from`/`to`) into the text it came
// from and concatenating the equal and deleted segments gives the old text back exactly, the equal and
// inserted ones the new text. The algorithm is Myers' O(ND) greedy diff; to keep a pathological input from
// stalling the page it is bounded by `MAX_EDIT_DISTANCE`, past which the whole file is reported as one
// replacement (still correct, just coarse). Same inputs always give the same segments: ties prefer deletion
// before insertion, as in the compiler's formatter.

/** Edit distance (deleted + inserted lines) the search explores before giving up on a minimal diff. */
export const MAX_EDIT_DISTANCE = 2000;

/** Splits `text` into lines that keep their `\n`; the last line has none when the text does not end in one. */
export function splitLines(text) {
  const lines = [];
  let start = 0;
  for (let index = text.indexOf('\n'); index !== -1; index = text.indexOf('\n', start)) {
    lines.push(text.slice(start, index + 1));
    start = index + 1;
  }
  if (start < text.length) {
    lines.push(text.slice(start));
  }
  return lines;
}

function myers(a, b, limit) {
  const n = a.length;
  const m = b.length;
  const max = Math.min(n + m, limit);
  const offset = max + 1;
  const v = new Int32Array(2 * max + 3);
  const trace = [];
  for (let d = 0; d <= max; d += 1) {
    trace.push(v.slice());
    for (let k = -d; k <= d; k += 2) {
      let x = k === -d || (k !== d && v[offset + k - 1] < v[offset + k + 1]) ? v[offset + k + 1] : v[offset + k - 1] + 1;
      let y = x - k;
      while (x < n && y < m && a[x] === b[y]) {
        x += 1;
        y += 1;
      }
      v[offset + k] = x;
      if (x >= n && y >= m) {
        return backtrack(trace, n, m, d, offset);
      }
    }
  }
  return null;
}

// Walks the saved frontiers back to a list of per-line ops, oldest first: 'e' equal, 'd' delete, 'i' insert.
function backtrack(trace, n, m, last, offset) {
  const ops = [];
  let x = n;
  let y = m;
  for (let d = last; d > 0; d -= 1) {
    const v = trace[d];
    const k = x - y;
    const down = k === -d || (k !== d && v[offset + k - 1] < v[offset + k + 1]);
    const previousK = down ? k + 1 : k - 1;
    const previousX = v[offset + previousK];
    const previousY = previousX - previousK;
    while (x > previousX && y > previousY) {
      ops.push('e');
      x -= 1;
      y -= 1;
    }
    ops.push(down ? 'i' : 'd');
    x = previousX;
    y = previousY;
  }
  while (x > 0 && y > 0) {
    ops.push('e');
    x -= 1;
    y -= 1;
  }
  return ops.reverse();
}

/**
 * The line diff from `before` to `after`.
 *
 * @typedef {{kind: 'equal' | 'delete' | 'insert', lines: string[], aLine: number, bLine: number,
 *   aFrom: number, aTo: number, bFrom: number, bTo: number}} DiffSegment
 *   (`aLine`/`bLine` are 0-based line indexes; `aFrom..aTo` and `bFrom..bTo` are UTF-16 offsets, and an
 *   insert has an empty `a` range, a delete an empty `b` range)
 * @param {string} before
 * @param {string} after
 * @param {number} [limit]
 * @returns {DiffSegment[]}
 */
export function diffLines(before, after, limit = MAX_EDIT_DISTANCE) {
  const a = splitLines(before);
  const b = splitLines(after);
  let ops = myers(a, b, limit);
  if (ops === null) {
    ops = [...a.map(() => 'd'), ...b.map(() => 'i')];
  }
  const segments = [];
  let ai = 0;
  let bi = 0;
  let aOffset = 0;
  let bOffset = 0;
  for (const op of ops) {
    const kind = op === 'e' ? 'equal' : op === 'd' ? 'delete' : 'insert';
    const line = op === 'i' ? b[bi] : a[ai];
    const last = segments[segments.length - 1];
    if (last === undefined || last.kind !== kind) {
      segments.push({ kind, lines: [], aLine: ai, bLine: bi, aFrom: aOffset, aTo: aOffset, bFrom: bOffset, bTo: bOffset });
    }
    const segment = segments[segments.length - 1];
    segment.lines.push(line);
    if (op !== 'i') {
      aOffset += line.length;
      ai += 1;
      segment.aTo = aOffset;
    }
    if (op !== 'd') {
      bOffset += line.length;
      bi += 1;
      segment.bTo = bOffset;
    }
  }
  return segments;
}

