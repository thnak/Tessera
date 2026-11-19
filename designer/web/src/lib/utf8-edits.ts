// SPDX-License-Identifier: MIT

// Applying a `source.patch` edit list to a text buffer (designer-protocol-v1 sections 3.3 and 10.2).
//
// The engine measures offsets and lengths in UTF-8 bytes (it holds the staged project as UTF-8), while a
// JavaScript string, and CodeMirror's document, count UTF-16 code units. This module is the one place the
// two meet, so a `source.patch` for a file with a non-ASCII character before the edit lands on the right
// text. It is plain TypeScript with no imports: the page and the Node gates call the same bytes.

/** One edit of a patch, as the engine reports it: byte offset and byte length into the file's text. */
export interface ByteEdit {
  offset: number;
  length: number;
  newText: string;
}

/** The same edit in UTF-16 code units, which is what a string slice and a CodeMirror change take. */
export interface TextChange {
  from: number;
  to: number;
  insert: string;
}

/** Why an edit list could not be converted: the page answers `source.patch.result` `rejected`. */
export class EditRangeError extends Error {}

/** The UTF-8 byte width of the code point that starts with UTF-16 unit `code`. */
function codePointBytes(code: number): number {
  if (code < 0x80) {
    return 1;
  }
  if (code < 0x800) {
    return 2;
  }
  // A high surrogate starts a pair, which is one four-byte code point.
  if (code >= 0xd800 && code <= 0xdbff) {
    return 4;
  }
  return 3;
}

/**
 * Converts byte ranges into UTF-16 ranges of `text`. A byte offset that is not on a code point boundary,
 * or lies past the end, throws `EditRangeError`: splitting a character would corrupt the file.
 */
export function toTextChanges(text: string, edits: readonly ByteEdit[]): TextChange[] {
  // One pass records the UTF-16 index of every byte offset the edits name.
  const wanted = new Set<number>();
  for (const edit of edits) {
    wanted.add(edit.offset);
    wanted.add(edit.offset + edit.length);
  }
  const index = new Map<number, number>();
  let bytes = 0;
  let unit = 0;
  while (unit <= text.length) {
    if (wanted.has(bytes)) {
      index.set(bytes, unit);
    }
    if (unit === text.length) {
      break;
    }
    const width = codePointBytes(text.charCodeAt(unit));
    bytes += width;
    unit += width === 4 ? 2 : 1;
  }
  return edits.map((edit) => {
    const from = index.get(edit.offset);
    const to = index.get(edit.offset + edit.length);
    if (from === undefined || to === undefined) {
      throw new EditRangeError(
        `the edit at byte ${edit.offset}+${edit.length} is not on a character boundary of the file`,
      );
    }
    return { from, to, insert: edit.newText };
  });
}

/**
 * `text` with `changes` applied. The changes address the ORIGINAL text and must not overlap; they are
 * applied from the highest offset down so a later edit never shifts an earlier one.
 */
export function applyChanges(text: string, changes: readonly TextChange[]): string {
  const ordered = [...changes].sort((a, b) => b.from - a.from || b.to - a.to);
  let next = text;
  let limit = text.length + 1;
  for (const change of ordered) {
    if (change.to > limit || change.from > change.to) {
      throw new EditRangeError("the edits of a patch overlap");
    }
    next = next.slice(0, change.from) + change.insert + next.slice(change.to);
    limit = change.from;
  }
  return next;
}
