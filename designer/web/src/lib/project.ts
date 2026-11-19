// SPDX-License-Identifier: MIT

// The project the left column shows (issue #431): the seeded sample project, and the tree rows derived
// from it.
//
// **Every path here has been through `project-paths.js`.** `confinePath` is the only place the studio
// decides what a project-relative path is (`designer/web/project-paths.js`, spec section 10), so the
// tree cannot show a path that a save, a zip entry or the compiler's `ModuleSource::file` would resolve
// differently. A path that escapes the project is refused by that module, with a code, rather than
// filtered out here by a second rule that could drift from it.
//
// **The seed is what makes the layout demonstrable without File System Access.** The studio's real load
// paths are a folder picker (Chromium only) and a zip (`studio-zip.js`); neither can be assumed, so the
// page boots with a small in-memory project -- an entry component, a second component, the stylesheet
// and `tessera.json` -- and every one of those paths goes through `confinePath` on its way into the
// tree, exactly as an opened folder or a dropped archive would. Wiring the two real load paths is the
// remaining half of #391 and is deliberately not claimed here.
//
// No DOM, no React: the tree component renders these rows, and the Node gates can build them.

import {
  baseName,
  classifyPath,
  confinePath,
  isTextKind,
  isViewOnlyKind,
  parentOf,
} from "../../project-paths.js";

/** One file of the opened project, as the editor and the tree see it. */
export interface ProjectFile {
  /** A canonical project-relative path, as `confinePath` returned it. */
  path: string;
  /** The last segment, for a tab or a row label. */
  name: string;
  /** `project-paths.js` `FILE_KIND`: source, stylesheet, config, theme, asset, other. */
  kind: string;
  /** The bytes as text; empty for a kind that is never read (an asset, a locked file). */
  text: string;
  /** `true` when the editor may show this file and Run may compile it. */
  editable: boolean;
  /**
   * `true` for a locked text file (C++, a generated header) that opens read-only when selected (owner
   * decision 2026-11-17). Its `text` stays empty: the lazily read copy is `viewText`, which no sender,
   * saver or exporter looks at.
   */
  viewOnly: boolean;
  /** The text of a view-only file once it was opened; `undefined` until then. */
  viewText?: string;
  /** Why a view-only file shows no text (`binary`, `too-large`, `unreadable`); `undefined` otherwise. */
  viewNote?: string;
  /** The file's `lastModified` at load (0 when unknown, a zip), for the "may be out of date" advice. */
  modified?: number;
}

/** One row of the tree: a directory the tree derives, or a file. */
export interface TreeRow {
  path: string;
  name: string;
  /** 0 for a top-level entry, 1 inside `ui/`, and so on. */
  depth: number;
  dir: boolean;
  /**
   * Directories only: whether the children are shown. A file is always `false`.
   *
   * It is on the row rather than left to the caller because the keyboard needs it (`ArrowRight` opens a
   * collapsed directory, `ArrowLeft` closes an open one) and a second lookup would be a second answer.
   */
  expanded: boolean;
  /** Files only: whether the editor may open it. */
  editable: boolean;
  /** Files only: a locked text file that opens read-only. */
  viewOnly: boolean;
  /** Files only: the `FILE_KIND`, for the locked badge and the tab's icon letter. */
  kind: string;
}

/**
 * The sample project the page boots with: a real project, not a single file.
 *
 * `tessera.json` names the entry (`app.entry`), the entry imports the stylesheet it is styled with, and
 * that import is the only way the sheet applies. There is no router yet: the scene builder does not render
 * user component instances, so a second screen cannot be composed in until the engine can.
 */
const SEED: readonly { path: string; text: string }[] = Object.freeze([
  {
    path: "tessera.json",
    text: `{
  "schemaVersion": 1,
  "app": { "name": "studio-demo", "entry": "ui/App.tsx", "namespace": "studio_demo" }
}
`,
  },
  {
    path: "ui/App.tsx",
    text: `import "../styles/theme.css";

export function App(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`,
  },
  {
    path: "styles/theme.css",
    text: `/* The project's author stylesheet: it applies because ui/App.tsx imports it. */
View {
  background: #ffffff;
}
`,
  },
]);

/** The file the page opens first: the entry of the seed project (it is what `tessera.json` names). */
export const SEED_ENTRY = "ui/App.tsx";

/**
 * Builds a project file from one path and its text.
 *
 * The path is confined here rather than by the caller, which is the point: whether it came from the
 * seed, a zip entry or a folder handle, the tree, the editor and the compiler see the canonical form
 * `confinePath` returned, or nothing at all.
 */
export function projectFile(path: string, text: string, context = {}): ProjectFile {
  const confined = confinePath(path);
  const kind = classifyPath(confined, context);
  const editable = isTextKind(kind);
  return {
    path: confined,
    name: baseName(confined),
    kind,
    // A locked kind (a font, an image, C++) is listed but never read: the spec's own sentence.
    text: editable ? text : "",
    editable,
    viewOnly: isViewOnlyKind(kind),
  };
}

/** The seeded project, every path confined. Throws if a seed path is not a project path. */
export function seedProject(): ProjectFile[] {
  return SEED.map((entry) => projectFile(entry.path, entry.text));
}

/** The directories of a project, derived from its file paths (a zip carries no empty directory). */
function directoriesOf(files: readonly ProjectFile[]): string[] {
  const dirs = new Set<string>();
  for (const file of files) {
    let dir = parentOf(file.path);
    while (dir !== "") {
      dirs.add(dir);
      dir = parentOf(dir);
    }
  }
  // Confined like every other path the tree shows, so a directory row is held to the same rule.
  return [...dirs].map((dir) => confinePath(dir)).sort();
}

/** The children of `dir` (the project root when `dir` is `""`), directories first, then files. */
function childrenOf(
  dir: string,
  dirs: readonly string[],
  files: readonly ProjectFile[],
  collapsed: ReadonlySet<string>,
): TreeRow[] {
  const rows: TreeRow[] = [];
  for (const candidate of dirs) {
    if (parentOf(candidate) !== dir) {
      continue;
    }
    rows.push({
      path: candidate,
      name: baseName(candidate),
      depth: 0,
      dir: true,
      expanded: !collapsed.has(candidate),
      editable: false,
      viewOnly: false,
      kind: "dir",
    });
  }
  for (const file of files) {
    if (parentOf(file.path) !== dir) {
      continue;
    }
    rows.push({
      path: file.path,
      name: file.name,
      depth: 0,
      dir: false,
      expanded: false,
      editable: file.editable,
      viewOnly: file.viewOnly,
      kind: file.kind,
    });
  }
  // Files sorted by name, so the tree does not depend on the order the project was seeded (or read from
  // a zip): the directories above are sorted by path, and a tree whose two halves sort differently reads
  // as a bug.
  return rows.sort((left, right) => {
    if (left.dir !== right.dir) {
      return left.dir ? -1 : 1;
    }
    return left.name < right.name ? -1 : left.name > right.name ? 1 : 0;
  });
}

/**
 * The rows the tree draws, depth-first, with the children of a collapsed directory left out.
 *
 * A collapsed directory stays a row (it is how it is reopened); its subtree is what disappears. The
 * expansion set holds canonical directory paths, so a row cannot be collapsed under one spelling and
 * matched under another.
 */
export function treeRows(
  files: readonly ProjectFile[],
  collapsed: ReadonlySet<string> = new Set(),
): TreeRow[] {
  const dirs = directoriesOf(files);
  const rows: TreeRow[] = [];
  const walk = (dir: string, depth: number): void => {
    for (const row of childrenOf(dir, dirs, files, collapsed)) {
      rows.push({ ...row, depth });
      if (row.dir && row.expanded) {
        walk(row.path, depth + 1);
      }
    }
  };
  walk("", 0);
  return rows;
}

/** A file's project record by path, or `null` when the project does not have it. */
export function fileAt(files: readonly ProjectFile[], path: string): ProjectFile | null {
  return files.find((file) => file.path === path) ?? null;
}
