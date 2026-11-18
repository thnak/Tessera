// SPDX-License-Identifier: MIT

// The studio's project load and save, with no DOM and no imports beyond the two sibling modules
// (issue #391, D-S4 of docs/specs/designer-studio-v1.md section 7).
//
// Three things arrive here the same way: a folder (File System Access), a zip, and a path list a test
// builds. Each is reduced to *raw entries* -- `{ path, size, lastModified, read }` where `read` is only
// called for a file the project accepts as text -- and `buildProject` is the single place that decides
// what a project is: it confines every path (`project-paths.js`), classifies it, applies the owner's
// limits **before** a byte is read, and reads nothing for a locked file (a font, an image, C++). So a
// folder and the zip of that folder produce the same project, and a hostile one is a named refusal
// rather than a half-loaded tree.
//
// The folder handle is used through its public methods only (`entries`, `getFile`, `getFileHandle`,
// `getDirectoryHandle`, `createWritable`), so the Node gate drives this module with an in-memory
// fake and the page passes the real `FileSystemDirectoryHandle` unchanged.

import {
  LIMITS,
  classifyPath,
  confinePath,
  isTextKind,
  limitMessage,
} from './project-paths.js';
import { readZip } from './studio-zip.js';

/** A refusal the page branches on by `code`: `limit` | `duplicate` | `permission` | `not-accepted`. */
export class ProjectIoError extends Error {
  /**
   * @param {string} code
   * @param {string} message a sentence for a person
   * @param {string} [limit] for `limit`: which `LIMITS` key was exceeded
   */
  constructor(code, message, limit = '') {
    super(message);
    this.name = 'ProjectIoError';
    this.code = code;
    this.limit = limit;
  }
}

function limitError(limit, actual) {
  return new ProjectIoError('limit', limitMessage(limit, actual), limit);
}

// Directories that are never walked: a hostile or merely large folder floods the tree there, and the
// spec lists `node_modules` and build output as locked anyway. Each shows as one locked row.
const OPAQUE_DIRS = new Set(['node_modules', '.git', 'build', 'dist', 'out', '.vite']);

const decoder = new TextDecoder('utf-8');

/** `tessera.json`'s declared assets, which decide what the accepted-file rule means for a path. */
function contextOf(text) {
  try {
    const assets = JSON.parse(text)?.assets ?? {};
    const strings = (value) => (Array.isArray(value) ? value.filter((v) => typeof v === 'string') : []);
    return {
      themePath: typeof assets.theme === 'string' ? assets.theme : undefined,
      assetPaths: [...strings(assets.fonts), ...strings(assets.images)],
    };
  } catch {
    return {};
  }
}

/** @typedef {{path: string, kind: string, text: string, size: number, lastModified: number}} ProjectEntry */

/**
 * Builds the project from raw entries: confine, classify, check the limits, then read the text files.
 *
 * @param {Array<{path: string, size: number, lastModified?: number, read: () => Promise<Uint8Array>}>} raw
 * @returns {Promise<{entries: ProjectEntry[], context: object}>}
 */
export async function buildProject(raw) {
  if (raw.length > LIMITS.maxTreeEntries) {
    throw limitError('maxTreeEntries', raw.length);
  }
  const confined = new Map();
  for (const entry of raw) {
    const path = confinePath(entry.path);
    if (confined.has(path)) {
      throw new ProjectIoError('duplicate', `the project has the path ${JSON.stringify(path)} twice`);
    }
    confined.set(path, entry);
  }
  const config = confined.get('tessera.json');
  const context =
    config && config.size <= LIMITS.maxFileBytes ? contextOf(decoder.decode(await config.read())) : {};

  // Every limit is checked from sizes alone, so an over-limit project is refused before any read.
  let accepted = 0;
  let bytes = 0;
  const plan = [];
  for (const [path, entry] of confined) {
    const kind = classifyPath(path, context);
    const text = isTextKind(kind);
    if (text) {
      if (entry.size > LIMITS.maxFileBytes) {
        throw limitError('maxFileBytes', entry.size);
      }
      accepted += 1;
      bytes += entry.size;
      if (accepted > LIMITS.maxAcceptedFiles) {
        throw limitError('maxAcceptedFiles', accepted);
      }
      if (bytes > LIMITS.maxAcceptedBytes) {
        throw limitError('maxAcceptedBytes', bytes);
      }
    }
    plan.push({ path, kind, text, entry });
  }

  const entries = [];
  for (const { path, kind, text, entry } of plan) {
    entries.push({
      path,
      kind,
      // A locked file is listed with its size and never read: the spec's own sentence.
      text: text ? decoder.decode(await entry.read()) : '',
      size: entry.size,
      lastModified: entry.lastModified ?? 0,
    });
  }
  return { entries, context };
}

// --- folder -------------------------------------------------------------------------------------

/** Walks a directory handle into raw entries, stopping at the tree limit before it floods memory. */
async function walkDirectory(directory, prefix, out) {
  for await (const [name, handle] of directory.entries()) {
    const path = prefix === '' ? name : `${prefix}/${name}`;
    if (out.length >= LIMITS.maxTreeEntries) {
      throw limitError('maxTreeEntries', out.length + 1);
    }
    if (handle.kind === 'directory') {
      if (OPAQUE_DIRS.has(name)) {
        out.push({ path, size: 0, lastModified: 0, read: async () => new Uint8Array() });
      } else {
        await walkDirectory(handle, path, out);
      }
      continue;
    }
    const file = await handle.getFile();
    out.push({
      path,
      size: file.size,
      lastModified: file.lastModified,
      read: async () => new Uint8Array(await file.arrayBuffer()),
    });
  }
}

/**
 * Loads a project from a directory handle.
 *
 * @returns {Promise<{entries: ProjectEntry[], context: object, baseline: Map<string, number>}>} `baseline`
 *   is each accepted file's `lastModified` at load, which `findConflicts` compares on save.
 */
export async function loadDirectory(root) {
  const raw = [];
  await walkDirectory(root, '', raw);
  const project = await buildProject(raw);
  const baseline = new Map();
  for (const entry of project.entries) {
    if (isTextKind(entry.kind)) {
      baseline.set(entry.path, entry.lastModified);
    }
  }
  return { ...project, baseline };
}

// --- zip ----------------------------------------------------------------------------------------

/**
 * A zip made from a folder usually has that folder as its only top-level entry (`proj/tessera.json`).
 * When `tessera.json` is not at the root but is exactly one level down, that level is the project root.
 */
function stripWrapper(entries) {
  if (entries.some((entry) => entry.path === 'tessera.json')) {
    return entries;
  }
  const wrapper = entries.find((entry) => /^[^/]+\/tessera\.json$/.test(entry.path));
  if (!wrapper) {
    return entries;
  }
  const prefix = wrapper.path.slice(0, wrapper.path.indexOf('/') + 1);
  return entries.filter((entry) => entry.path.startsWith(prefix)).map((entry) => ({
    ...entry,
    path: entry.path.slice(prefix.length),
  }));
}

/** Loads a project from the bytes of a zip. Refuses an oversized archive before parsing it. */
export async function loadZip(bytes) {
  if (bytes.length > LIMITS.maxArchiveBytes) {
    throw limitError('maxArchiveBytes', bytes.length);
  }
  const zipped = stripWrapper(await readZip(bytes));
  return buildProject(
    zipped.map((entry) => ({
      path: entry.path,
      size: entry.bytes.length,
      lastModified: 0,
      read: async () => entry.bytes,
    })),
  );
}

// --- save ---------------------------------------------------------------------------------------

/** Walks to the file handle of `path` under `root`; `null` when it (or a directory above it) is absent. */
async function fileHandleAt(root, path, create) {
  const segments = confinePath(path).split('/');
  let directory = root;
  for (const segment of segments.slice(0, -1)) {
    try {
      directory = await directory.getDirectoryHandle(segment, { create });
    } catch (error) {
      if (!create && error?.name === 'NotFoundError') {
        return null;
      }
      throw error;
    }
  }
  try {
    return await directory.getFileHandle(segments[segments.length - 1], { create });
  } catch (error) {
    if (!create && error?.name === 'NotFoundError') {
      return null;
    }
    throw error;
  }
}

/** Asks for (or confirms) read/write permission on a handle; throws `permission` when it is refused. */
export async function ensureWritable(handle) {
  const mode = { mode: 'readwrite' };
  if (typeof handle.queryPermission === 'function' && (await handle.queryPermission(mode)) === 'granted') {
    return;
  }
  if (typeof handle.requestPermission === 'function' && (await handle.requestPermission(mode)) === 'granted') {
    return;
  }
  if (typeof handle.queryPermission !== 'function') {
    return; // a handle with no permission model (a test double) is writable
  }
  throw new ProjectIoError('permission', 'The browser did not grant write access to the folder.');
}

/**
 * The paths among `paths` whose file on disk changed since load: its `lastModified` differs from the
 * baseline, or it exists now although the project created it. A file that was deleted from disk is not a
 * conflict -- saving recreates it.
 *
 * @param {Iterable<string>} paths
 * @param {Map<string, number>} baseline
 * @returns {Promise<string[]>}
 */
export async function findConflicts(root, paths, baseline) {
  const conflicts = [];
  for (const path of paths) {
    const handle = await fileHandleAt(root, path, false);
    if (handle === null) {
      continue;
    }
    const file = await handle.getFile();
    if (!baseline.has(path) || baseline.get(path) !== file.lastModified) {
      conflicts.push(path);
    }
  }
  return conflicts;
}

/**
 * Writes the given files into the opened root and returns their new `lastModified` values.
 *
 * Only inside the root (every path is confined) and only accepted text kinds: this is the one place a
 * write happens, so the rule cannot be bypassed by a caller.
 *
 * @param {Array<{path: string, text: string, kind?: string}>} files
 * @returns {Promise<Map<string, number>>}
 */
export async function writeFiles(root, files) {
  await ensureWritable(root);
  const written = new Map();
  for (const { path, text, kind } of files) {
    if (!isTextKind(kind ?? classifyPath(confinePath(path)))) {
      throw new ProjectIoError('not-accepted', `${JSON.stringify(path)} is not a file the studio writes`);
    }
    const handle = await fileHandleAt(root, path, true);
    const writable = await handle.createWritable();
    try {
      await writable.write(text);
    } finally {
      await writable.close();
    }
    written.set(path, (await handle.getFile()).lastModified);
  }
  return written;
}

/** Reads one accepted text file from the opened root, for "take the disk's version" in a conflict. */
export async function readText(root, path) {
  const handle = await fileHandleAt(root, path, false);
  if (handle === null) {
    return null;
  }
  const file = await handle.getFile();
  if (file.size > LIMITS.maxFileBytes) {
    throw limitError('maxFileBytes', file.size);
  }
  return { text: decoder.decode(new Uint8Array(await file.arrayBuffer())), lastModified: file.lastModified };
}
