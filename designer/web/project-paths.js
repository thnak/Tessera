// SPDX-License-Identifier: MIT

// Project-relative paths and the confinement rule (issue #391, studio slice D-S4 of
// docs/specs/designer-studio-v1.md section 7).
//
// **Why this module exists on its own.** The studio reads files a person selected (a folder through the
// File System Access API, a dropped zip) and writes some of them back. Every path that reaches a
// `FileSystemDirectoryHandle`, a zip entry name, or the compiler's `ModuleSource::file` goes through
// `confinePath` first, so "never construct a path outside the opened folder" is one function with one
// test rather than a rule every caller re-derives. Section 10 of the spec states the rule: an absolute
// path, `..`, a symlink (zip) and a duplicate path are refused, each with a named error, because a
// hostile folder or zip must not escape the virtual project root.
//
// **The canonical form** is the one `ModuleSource::file` uses (src/compiler/lowering/include/
// tessera/compiler/lowering/node_lowering.hpp:47): project-relative, POSIX separators, no leading
// `./`. The C++ side normalizes a Windows `\\` to `/` and strips `./`; this side *refuses* a `\\`
// instead, because the studio is the boundary: a zip written by an archiver never needs one
// (APPNOTE 4.4.17.1 requires `/`), and a name that carries one is either a Windows absolute path
// escape or a malformed entry, and both are refused by the spec's own list.
//
// Nothing here touches the DOM or the filesystem: the same functions decide what a zip may contain,
// what a file tree may show, and what a save may write. That is deliberate -- this module and
// `studio-zip.js` are the two framework-independent pieces of #391, with no imports and no `window`,
// so a shell (the React one #422 brings, or the current page) imports them unchanged.

// The real limits, not `ui.limits.*` (documented but unread, issue #405). The per-file cap is the
// compiler's own `TESSERA_JSX_LIMIT_FILE_SIZE` (docs/specs/jsx-subset-v1-jsx.md section 15, 1 MiB),
// so a file this module accepts is a file the front end will read rather than refuse one stage later.
// The other numbers are the owner's decision of 2026-11-16 in issue #391 (500 files, 1 MB per file,
// 20 MB in total), with the per-file figure read as 1 MiB because the spec's section 7 defers to the
// jsx-subset limit, and the tree cap added because the *tree* is the thing a hostile folder can
// flood (`node_modules` is listed locked, so the entry count is not the accepted-file count).
export const LIMITS = Object.freeze({
  /** The compiler's per-file limit (`TESSERA_JSX_LIMIT_FILE_SIZE`): 1 MiB. */
  maxFileBytes: 1048576,
  /** Accepted (text) files in one project. */
  maxAcceptedFiles: 500,
  /** Accepted (text) bytes in one project: the owner's 20 MB, kept in MiB for consistency. */
  maxAcceptedBytes: 20 * 1024 * 1024,
  /** Entries the tree will list, accepted or locked. */
  maxTreeEntries: 5000,
  /** Bytes of a dropped zip. Read before anything is parsed, from `File.size` when available. */
  maxArchiveBytes: 64 * 1024 * 1024,
  /** Bytes in one project-relative path. */
  maxPathBytes: 512,
  /** `*.mock.ts` files in one project (docs/specs/studio-mock-services-v1.md section 2). */
  maxMockFiles: 64,
  /** Bytes of `*.mock.ts` text in one project: 8 MiB (each file is also bound by `maxFileBytes`). */
  maxMockBytes: 8 * 1024 * 1024,
});

/** A refusal a shell branches on by `code`, never by matching a message. */
export class PathError extends Error {
  /**
   * @param {string} code `absolute` | `traversal` | `backslash` | `empty` | `control` |
   *   `too-long` | `duplicate` | `symlink` | `reserved` | `too-many` | `too-large`
   * @param {string} message a sentence for a person, naming the path
   */
  constructor(code, message) {
    super(message);
    this.name = 'PathError';
    this.code = code;
  }
}

const CONTROL = /[\u0000-\u001f\u007f]/;

// Windows device names are not files: opening `NUL` or `CON` succeeds and writes nowhere, and a
// project cannot contain them. Refused on every platform so a project behaves the same everywhere.
const RESERVED_NAMES = new Set([
  'con', 'prn', 'aux', 'nul',
  'com1', 'com2', 'com3', 'com4', 'com5', 'com6', 'com7', 'com8', 'com9',
  'lpt1', 'lpt2', 'lpt3', 'lpt4', 'lpt5', 'lpt6', 'lpt7', 'lpt8', 'lpt9',
]);

/**
 * Confines one project-relative path and returns its canonical form.
 *
 * Throws `PathError` naming the path and what is wrong with it. The checks are deliberately ordered
 * from the most specific refusal to the most general, so a message names the actual problem: an
 * absolute path says `absolute`, not `traversal`.
 *
 * @param {string} name a zip entry name, a `FileSystemHandle.name` joined to its parent, or a
 *   path a shell constructed
 * @returns {string} the canonical project-relative path
 */
export function confinePath(name) {
  if (typeof name !== 'string' || name === '') {
    throw new PathError('empty', 'a path must be a non-empty string');
  }
  if (CONTROL.test(name)) {
    throw new PathError('control', `the path ${quote(name)} contains a control character`);
  }
  if (name.includes('\\')) {
    throw new PathError(
      'backslash',
      `the path ${quote(name)} contains a backslash; a project path is POSIX-separated`,
    );
  }
  if (name.startsWith('/')) {
    throw new PathError('absolute', `the path ${quote(name)} is absolute`);
  }
  if (/^[A-Za-z]:/.test(name)) {
    throw new PathError('absolute', `the path ${quote(name)} names a drive`);
  }
  const segments = [];
  for (const part of name.split('/')) {
    if (part === '' || part === '.') {
      continue;
    }
    if (part === '..') {
      throw new PathError(
        'traversal',
        `the path ${quote(name)} contains "..", which would leave the project`,
      );
    }
    // `COM1.tsx` is still `COM1`: Windows resolves the device from the name before the first dot,
    // and a trailing dot or space is stripped by the Win32 layer before that. `part.toLowerCase()`
    // is deliberate -- a project that opens on Windows and on Linux must classify a path the same.
    const stem = part.toLowerCase().replace(/[ .]+$/, '').split('.')[0];
    if (RESERVED_NAMES.has(stem)) {
      throw new PathError('reserved', `the path ${quote(name)} names a reserved device`);
    }
    segments.push(part);
  }
  if (segments.length === 0) {
    throw new PathError('empty', `the path ${quote(name)} names no file`);
  }
  const canonical = segments.join('/');
  if (canonical.length > LIMITS.maxPathBytes) {
    throw new PathError(
      'too-long',
      `the path ${quote(name)} is longer than ${LIMITS.maxPathBytes} bytes`,
    );
  }
  return canonical;
}

/** `true` when `confinePath` accepts `name`; the predicate form, for a filter. */
export function isConfined(name) {
  try {
    confinePath(name);
    return true;
  } catch {
    return false;
  }
}

/** The parent directory of a canonical path, or `''` for a file at the project root. */
export function parentOf(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? '' : path.slice(0, slash);
}

/** The last segment of a canonical path. */
export function baseName(path) {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.slice(slash + 1);
}

/** The lowercase extension including the dot, or `''`. */
export function extensionOf(path) {
  const base = baseName(path);
  const dot = base.lastIndexOf('.');
  return dot <= 0 ? '' : base.slice(dot).toLowerCase();
}

/**
 * Joins `base` and a relative specifier and collapses `.`/`..`, mirroring the C++ `join_project_path`
 * (src/compiler/lowering/src/project_path.cpp) that the compiler resolves imports with.
 *
 * A `..` that leaves the project root is dropped rather than followed: the C++ function pops only
 * when there is something to pop, and the studio must agree with the compiler about which file an
 * import names, so the same rule (and not a stricter one) is applied here. The result is then
 * confined, so a specifier that climbs above the root cannot address a file outside it.
 */
export function joinProjectPath(base, relative) {
  const joined = base === '' ? String(relative) : `${base}/${relative}`;
  const parts = [];
  for (const part of joined.split('/')) {
    if (part === '..') {
      if (parts.length > 0) {
        parts.pop();
      }
    } else if (part !== '' && part !== '.') {
      parts.push(part);
    }
  }
  return parts.join('/');
}

/** `true` when `path` is `dir` or sits under it. */
export function isUnder(path, dir) {
  if (dir === '') {
    return true;
  }
  return path === dir || path.startsWith(dir + '/');
}

/** The accepted-file kinds of spec section 7, and `other` for everything the tree shows locked. */
export const FILE_KIND = Object.freeze({
  source: 'source',
  stylesheet: 'stylesheet',
  config: 'config',
  theme: 'theme',
  asset: 'asset',
  // A text file the studio does not accept (C++, a generated header, a README): listed with a lock, opened
  // view-only on demand (owner decision 2026-11-17), and never sent to the viewer or written back.
  viewOnly: 'view-only',
  // A design-time mock service (`*.mock.ts`, ADR-055). Text and editable, but host-only: the viewer is
  // only ever given `source` and `stylesheet` files, so a mock never reaches the engine.
  mock: 'mock',
  other: 'other',
});

// The kinds the viewer is given and the editor may write. A font or an image is an accepted file of
// the *project* but never a text file: it is listed (with its size) and never read into memory
// beyond that, which is the spec's own sentence.
const TEXT_KINDS = new Set([
  FILE_KIND.source,
  FILE_KIND.stylesheet,
  FILE_KIND.config,
  FILE_KIND.theme,
  FILE_KIND.mock,
]);

/** `true` when a file of this kind is read as text, sent to the viewer and editable. */
export function isTextKind(kind) {
  return TEXT_KINDS.has(kind);
}

// Extensions of text a developer may want to read next to the UI source. A match only makes a locked file
// *openable*; it never makes it a text kind, so no byte of it reaches the viewer or a save.
const VIEW_ONLY_EXTENSIONS = new Set([
  '.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp', '.c', '.cc', '.cpp', '.cxx', '.ixx',
  '.ts', '.js', '.mjs', '.cjs', '.json', '.md', '.txt', '.yml', '.yaml', '.toml', '.cmake',
  '.html', '.xml', '.glsl', '.vert', '.frag', '.sh', '.ps1', '.py', '.cs', '.rs', '.go',
]);

/** `true` when a file of this kind opens read-only as text (and is still never sent or written). */
export function isViewOnlyKind(kind) {
  return kind === FILE_KIND.viewOnly;
}

/**
 * Classifies one confined path.
 *
 * `context` carries what `tessera.json` declares, because the spec's accepted list is partly
 * config-driven: `themePath` is `assets.theme` and `assetPaths` are `assets.fonts`/`assets.images`.
 * A path that is not named there and is not `.tsx`/`.jsx`/`.css`/`tessera.json` is `other`: shown
 * with a lock badge, never read, never sent to the viewer. That is what keeps C++, generated files,
 * `node_modules` and build output out of the engine (ADR-040: the developer class is never edited).
 *
 * @param {string} path a canonical project-relative path
 * @param {{ themePath?: string, assetPaths?: Iterable<string> }} [context]
 */
export function classifyPath(path, context = {}) {
  const extension = extensionOf(path);
  if (path === 'tessera.json') {
    return FILE_KIND.config;
  }
  if (extension === '.tsx' || extension === '.jsx') {
    return FILE_KIND.source;
  }
  if (extension === '.css') {
    return FILE_KIND.stylesheet;
  }
  // The extension is the rule, not the directory (ADR-055 decision 1): `mocks/` is a convention, and no
  // other `.ts` file is accepted, so a plain `util.ts` stays `other` (locked).
  if (/\.mock\.ts$/i.test(path)) {
    return FILE_KIND.mock;
  }
  if (context.themePath && path === context.themePath) {
    return FILE_KIND.theme;
  }
  if (extension === '.json' && /(^|\/)[^/]*theme[^/]*\.json$/i.test(path)) {
    // A theme asset the config has not named yet: still a project JSON a person may be writing.
    // It is only *sent* once `assets.theme` points at it, which is why `theme` is decided above by
    // the config and this branch is a visible-but-locked kind rather than an editable one.
    return FILE_KIND.theme;
  }
  if (context.assetPaths) {
    for (const asset of context.assetPaths) {
      if (path === asset || isUnder(path, asset)) {
        return FILE_KIND.asset;
      }
    }
  }
  if (/\.(png|jpe?g|webp|gif|bmp|ico|ttf|otf|woff2?)$/i.test(path)) {
    return FILE_KIND.asset;
  }
  // Build output and dependency folders are listed as one locked row (project-io `OPAQUE_DIRS`); a zip can
  // still carry files under them, and those stay name-and-size only rather than opening as text.
  if (/(^|\/)(node_modules|\.git|build|dist|out|\.vite)\//.test(path)) {
    return FILE_KIND.other;
  }
  if (VIEW_ONLY_EXTENSIONS.has(extension) || /(^|\/)(CMakeLists\.txt|Makefile|LICENSE)$/.test(path)) {
    return FILE_KIND.viewOnly;
  }
  return FILE_KIND.other;
}

/** The message form of a limit refusal, so a shell and the tests spell it once. */
export function limitMessage(limit, actual) {
  switch (limit) {
    case 'maxAcceptedFiles':
      return `The project has more than ${LIMITS.maxAcceptedFiles} files that can be edited.`;
    case 'maxAcceptedBytes':
      return `The project's editable files total more than ${LIMITS.maxAcceptedBytes} bytes.`;
    case 'maxTreeEntries':
      return `The project has more than ${LIMITS.maxTreeEntries} entries to list.`;
    case 'maxArchiveBytes':
      return `The archive is larger than ${LIMITS.maxArchiveBytes} bytes.`;
    case 'maxMockFiles':
      return `The project has more than ${LIMITS.maxMockFiles} mock files (TESSERA_MOCK_FILE_LIMIT).`;
    case 'maxMockBytes':
      return `The project's mock files total more than ${LIMITS.maxMockBytes} bytes (TESSERA_MOCK_TOTAL_TOO_LARGE).`;
    case 'maxFileBytes':
      return `A file is larger than ${LIMITS.maxFileBytes} bytes (${actual} bytes).`;
    default:
      return `A project limit was exceeded (${limit}).`;
  }
}

function quote(text) {
  return JSON.stringify(String(text));
}
