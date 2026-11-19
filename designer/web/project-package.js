// SPDX-License-Identifier: MIT

// The studio's project package (issue #391, owner decision 2026-11-17): an ordinary zip that also carries a
// small manifest, so a project that travels as a file keeps what a zip alone loses -- each file's
// modification time, size and (optionally) a checksum. The times are what the "generated file may be out of
// date" advice (`generated-staleness.js`) and change detection need; a plain zip has none.
//
// The format is specified in docs/design/studio-project-io.md section 7. In short: `tessera-project.json`
// at the root, `{ "format": "tessera-project", "version": 1, "files": { "<path>": [seconds, size, crc32?] } }`.
// Unknown keys and extra array items are ignored (forward compatible); an unknown *version* is refused with a
// message, because a reader cannot know what a newer writer meant. A plain zip has no manifest and still
// opens, with unknown times.
//
// No DOM and no wall clock: the writer is handed each file's time, so the same project writes the same bytes.

import { crc32, writeZip } from './studio-zip.js';

export const MANIFEST_PATH = 'tessera-project.json';
export const FORMAT = 'tessera-project';
/** The newest manifest version this reader understands. */
export const VERSION = 1;

/** A refusal the page shows by `code`: `format` | `version` | `malformed` | `size` | `hash`. */
export class PackageError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'PackageError';
    this.code = code;
  }
}

const hex = (value) => (value >>> 0).toString(16).padStart(8, '0');

/**
 * The manifest bytes for `files` (`{ path, bytes, modified }`, `modified` in epoch milliseconds, 0 when
 * unknown). Times are whole seconds; keys are sorted so the output is deterministic.
 */
export function buildManifest(files) {
  const table = {};
  for (const file of [...files].sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0))) {
    table[file.path] = [Math.max(0, Math.floor((file.modified ?? 0) / 1000)), file.bytes.length, hex(crc32(file.bytes))];
  }
  return new TextEncoder().encode(`${JSON.stringify({ format: FORMAT, version: VERSION, files: table })}\n`);
}

/**
 * Splits the manifest out of freshly read zip entries and checks the rest against it.
 *
 * @param {Array<{path: string, bytes: Uint8Array}>} entries
 * @returns {{entries: Array<{path: string, bytes: Uint8Array, modified: number}>, packaged: boolean}}
 *   `entries` without the manifest, each with `modified` in ms (0 when the manifest does not list it)
 * @throws {PackageError}
 */
export function applyManifest(entries) {
  const manifest = entries.find((entry) => entry.path === MANIFEST_PATH);
  const rest = entries.filter((entry) => entry.path !== MANIFEST_PATH);
  if (manifest === undefined) {
    return { entries: rest.map((entry) => ({ ...entry, modified: 0 })), packaged: false };
  }
  let parsed;
  try {
    parsed = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(manifest.bytes));
  } catch {
    throw new PackageError('malformed', `${MANIFEST_PATH} is not valid JSON.`);
  }
  if (parsed === null || typeof parsed !== 'object' || parsed.format !== FORMAT) {
    throw new PackageError('format', `${MANIFEST_PATH} is not a Tessera project manifest.`);
  }
  if (!Number.isInteger(parsed.version) || parsed.version < 1) {
    throw new PackageError('malformed', `${MANIFEST_PATH} has no valid version.`);
  }
  if (parsed.version > VERSION) {
    throw new PackageError(
      'version',
      `This project package is version ${parsed.version}; this studio reads up to version ${VERSION}. Update the studio to open it.`,
    );
  }
  const table = parsed.files;
  if (table === null || typeof table !== 'object' || Array.isArray(table)) {
    throw new PackageError('malformed', `${MANIFEST_PATH} has no "files" table.`);
  }
  return {
    packaged: true,
    entries: rest.map((entry) => {
      const row = Object.hasOwn(table, entry.path) ? table[entry.path] : undefined;
      if (row === undefined) {
        return { ...entry, modified: 0 };
      }
      if (!Array.isArray(row) || !Number.isInteger(row[0]) || row[0] < 0 || !Number.isInteger(row[1])) {
        throw new PackageError('malformed', `${MANIFEST_PATH} has a malformed entry for ${JSON.stringify(entry.path)}.`);
      }
      if (row[1] !== entry.bytes.length) {
        throw new PackageError(
          'size',
          `${JSON.stringify(entry.path)} is ${entry.bytes.length} bytes but the manifest says ${row[1]}: the package was changed after it was written.`,
        );
      }
      if (typeof row[2] === 'string' && row[2] !== hex(crc32(entry.bytes))) {
        throw new PackageError('hash', `${JSON.stringify(entry.path)} does not match its checksum in the manifest.`);
      }
      return { ...entry, modified: row[0] * 1000 };
    }),
  };
}

/** The package zip for `files` (`{ path, bytes, modified }`): the files plus their manifest. */
export function writePackage(files) {
  return writeZip([
    ...files.map(({ path, bytes }) => ({ path, bytes })),
    { path: MANIFEST_PATH, bytes: buildManifest(files) },
  ]);
}
