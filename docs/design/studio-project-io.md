# Studio project IO (issue #391): zip container and path confinement

Status: landed as two framework-independent modules, waiting to be wired into the React shell (#422).
Issue: [#391](https://github.com/thnak/Tessera/issues/391), studio slices D-S2 and D-S4.
Spec: [`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 7 (project load, edit and save)
and section 10 (the path rule).
Predecessors: [`studio-page.md`](studio-page.md) (the merged page this was going to extend).

The owner's decision of 2026-11-16 made the studio an IDE in the browser: open a folder of a UI project,
edit it through a file tree, save back. That is D-S2 (the shell) and D-S4 (project IO). **ADR-054**
(issue #422) then made the front end React 19 + Vite, and `AGENTS.md` now lists #422 as blocking #391,
so the shell work waits for that rewrite.

Two pieces of #391 are not about the shell at all, and they are the two the issue cannot be accepted
without: the **zip container** (the load and export route in every browser without the File System
Access API) and the **path-confinement rule** (the one place the feature can do real harm, because it
reads arbitrary files a person selected and writes some back). Both are plain ES modules with no
imports and no DOM, so the React shell imports them unchanged. This note is their contract, so the next
lane does not re-derive it.

## 1. The zip container: `designer/web/studio-zip.js`

```js
import { writeZip, readZip, crc32, ZipError } from './studio-zip.js';

const bytes = writeZip([{ path: 'ui/App.tsx', bytes: new TextEncoder().encode(text) }]);
const entries = await readZip(bytes);   // [{ path, bytes }]
```

**`writeZip(entries) -> Uint8Array`.** STORE only (method 0), because a preview project is text and
deflate would be a compression implementation for a few hundred kilobytes. Deterministic: entries are
sorted by path and the timestamp is fixed at 1980-01-01, so the same project writes the same bytes twice.
That is a requirement, not a nicety: it is what lets "Download project" produce a comparable archive and
what lets the round-trip test compare bytes rather than "equivalent archives".

**`readZip(bytes) -> Promise<[{ path, bytes }]>`.** STORE **and** DEFLATE. A zip a person actually has
came from Finder, Explorer or `zip -r` and is deflated, so a STORE-only reader would refuse almost every
real file. The inflater is the platform's `DecompressionStream('deflate-raw')` (the same primitive
`share.js` uses for the share link), so there is still no dependency. Every entry's CRC-32 is verified.

**`crc32(bytes) -> number`** is exported because writing a zero there produces a file that opens in
some readers and fails in others.

**Every refusal is a named `ZipError`**, never a half-loaded project. A shell branches on `code`:

| Case | `code` |
|---|---|
| a corrupt or truncated body | `crc` / `truncated` |
| an encrypted entry | `encrypted` |
| a compression method nothing here reads | `method` |
| a Unix symlink entry (a path out of the archive) | `symlink` |
| the same entry name twice | `duplicate` |
| a ZIP64 archive | `zip64` |
| a file that is not a zip | `signature` |
| a body that inflates past its declared size | `crc` |

The last row is the zip-bomb guard: the central directory declares the exact uncompressed size, so the
reader stops the stream one byte past it and cancels the decompressor, instead of buffering a gigabyte
before the CRC could disagree.

**What it deliberately does not do: path safety.** `readZip` returns entry names verbatim and says
nothing about whether they are safe. Confinement is one decision in one place (section 2), so a caller
must run every entry through `confinePath` before using it. This is why the two modules are separate
files.

## 2. Path confinement: `designer/web/project-paths.js`

**The rule.** Every project-relative path the studio handles — a zip entry name, a
`FileSystemHandle` name joined to its parent, a path a shell constructed, a path a save will write —
must name a file *inside* the opened project folder. `confinePath` is that rule and the only place it is
decided.

```js
import { confinePath, PathError, isConfined } from './project-paths.js';

confinePath('ui/App.tsx');    // 'ui/App.tsx'
confinePath('./ui//App.tsx'); // 'ui/App.tsx'  (normalized)
confinePath('../outside.tsx'); // throws PathError { code: 'traversal' }
confinePath('/etc/passwd');    // throws PathError { code: 'absolute' }
```

**The canonical form** is the compiler's `ModuleSource::file` form
(`include/tessera/compiler/lowering/node_lowering.hpp`): project-relative, POSIX separators, no leading
`./`. A string that reaches a `FileSystemDirectoryHandle`, a zip entry or the compiler is this form.

**What it refuses, with the code a message names:**

| Input | `code` | Why |
|---|---|---|
| `/etc/passwd`, `C:/Windows/...` | `absolute` | outside the root |
| `../outside.tsx`, `ui/../..` | `traversal` | leaves the root |
| `ui\App.tsx`, `\\server\share` | `backslash` | a Windows escape or a malformed entry; APPNOTE 4.4.17.1 requires `/` |
| `NUL`, `con`, `ui/COM1.tsx` | `reserved` | a Win32 device, not a file; the stem before the first dot is what Windows resolves |
| `ui/\u0000App.tsx` | `control` | not a path |
| `''`, `'.'`, `'./'` | `empty` | names no file |
| longer than `LIMITS.maxPathBytes` | `too-long` | refused, never truncated |

**The property a caller leans on**, asserted over the whole accepted corpus in the test: a canonical path
never starts with `/`, never contains `..`, never contains a backslash, and canonicalizing it again
changes nothing. That is what makes "never construct a path outside the opened folder" true by
construction rather than by review.

**Agreement with the compiler.** `joinProjectPath(base, specifier)` mirrors the C++
`join_project_path` (`src/compiler/lowering/src/project_path.cpp`), so the file the studio thinks an
import names is the file the compiler reads. A disagreement here would be a preview of a different file
than the build compiles, so the cases are copied from the C++ function.

**The accepted-file rule.** `classifyPath(path, context) -> FILE_KIND` is the spec's section 7 set:
`source` (`.tsx`/`.jsx`), `stylesheet` (`.css`), `config` (`tessera.json`), `theme` (`assets.theme`), and
`asset` (a font or image the config names). Everything else — C++, generated output, `node_modules`,
build output, a stray binary — is `other`, `isTextKind(kind)` is false, and that single decision produces
the tree's lock badge *and* the "never sent to the viewer, never written" guarantee. **Superseded in part
by the owner's decision of 2026-11-17** (`designer-studio-v1.md` section 7): a locked text file is
**viewable**, read lazily when opened and under the per-file cap, so "never read into memory beyond its
name and size" no longer holds for it; the classifier needs a view-only text kind beside `other`, and a
binary stays name and size only. ADR-040's "the developer class is never edited" is therefore one branch, not a check
the UI has to remember.

**The limits are the real ones**, not `ui.limits.*` (documented but unread, issue #405):
`maxFileBytes` is the compiler's own `TESSERA_JSX_LIMIT_FILE_SIZE` (1 MiB), so a file this module
accepts is a file the front end will read rather than refuse one stage later. The rest are the owner's
figures of 2026-11-16 in issue #391 (500 files, 20 MiB total), plus `maxTreeEntries` (a tree a hostile
folder cannot flood) and `maxArchiveBytes`. `limitMessage(key)` gives each a sentence that names it, so a
refusal is never "a limit was exceeded".

## 3. What the React shell (#422) needs from this layer

The shell has to build the tree, the editor, the save and the export. What it can take from here without
re-deciding anything:

1. **Confinement on every way in.** Call `confinePath` on a zip entry name before using it, and on each
   `FileSystemHandle.name` joined to its parent directory as the folder walk descends. A handle whose
   name is refused should stop the walk with the `PathError`'s code rather than be skipped silently — a
   skipped directory is a project that looks complete and is not.
2. **Confinement on the way out.** A save must re-confine each path it writes and resolve it from the
   root by path (split the confined result on `/` and walk `getDirectoryHandle`/`getFileHandle`), never
   from a handle remembered at load. Then a renamed or removed directory in the in-memory model cannot
   redirect a write. `writeProject` in the deferred pane did exactly this and is worth re-deriving the
   same way.
3. **The accepted set decides reading, editing and sending.** `classifyPath` + `isTextKind`: a path that
   is not a text kind is a row with a size and a lock. Nothing else should have an opinion, or the tree
   and the viewer will disagree about what is locked.
4. **`readZip`/`writeZip` for the zip route**, and `ZipError.code` for the message. `writeZip` needs the
   *current* text of every editable file, so an unsaved edit is in the export.
5. **`LIMITS` and `limitMessage`** for the size and count refusals, so the numbers live in one place.
6. **`extensionOf`, `parentOf`, `baseName`, `isUnder`** for the tree: the folder view is derived from a
   flat path list, so the zip route and the folder route produce the same tree for the same project.

What the shell must still decide, and this layer does not: which file is the entry (`tessera.json`'s
`app.entry`, else the first `.tsx` in path order); what a save writes (only the files that changed);
when it writes (an explicit action — there is no autosave anywhere in the studio); and how a conflict is
presented (keep mine / take disk / cancel, before writing anything).

## 4. Tests

Both are Node, need no browser, and are the only verification this change has:

| Command | Cases | Covers |
|---|---|---|
| `node scripts/ci/preview_project_io.mjs` | 16 | the zip round trip, determinism, binary data, a deflated entry, and all eight named refusals including the zip bomb |
| `node scripts/ci/preview_project_paths.mjs` | 41 | every confinement case as a table, the total/closed properties, `joinProjectPath` against the C++ function, `classifyPath`, `isTextKind`, the limits, and the no-DOM/no-imports claim |

Both suites read the module's own source for the last assertion, so "the React shell imports this
unchanged" is checked rather than described.

## 5. Not implemented, by decision

The shell work is **deferred to #422**, which blocks #391: the file tree widget, the per-file editor
binding, the debounced re-preview, the save/conflict UI, the drop handler, the multi-file resolution
plumbing into the page (`tessera_project_json` and a `studio_project.cpp` driver were prototyped and
reverted with the page work), the `PAGE_FILES` rows, and any budget re-record. None of it was landed:
`git status` on that lane shows only the four files of this note's two modules and their two tests, and
no page file is modified.

Browser verification: **not performed.** Opening a folder, dropping a zip on a page and starting a
download all need a page, and the page is blocked on #422.
