// SPDX-License-Identifier: MIT

// The workspace's React state (issue #431): the opened project, the open tabs, the active file and the
// tree's selection.
//
// **Why the buffers are in a ref and not in state.** A textarea's `onInput` fires on every keystroke, and
// putting the text in React state would re-render the tree, the tabs, the preview column and the canvas
// host on every character typed. The text is therefore a `Map` in a ref -- the editor's own storage --
// and React's state is only what *changes the shape of the page*: which files are open, which is active,
// which rows are collapsed and which files are dirty. A buffer write marks a file dirty once, so typing
// costs one re-render in total rather than one per key.
//
// **What this module does not own.** The Wasm sessions, the WebGPU device and the compile itself stay in
// `views/*-session.ts`; the page hands them a `SourceHost` (`lib/views.ts`) that reads the file Run
// compiles. Nothing here touches the DOM or React beyond the hook.

import { useCallback, useMemo, useRef, useState } from "react";
import {
  SEED_ENTRY,
  fileAt,
  seedProject,
  treeRows,
  type ProjectFile,
  type TreeRow,
} from "./lib/project";
import { entryOf, type EntryResult } from "./lib/project-snapshot";

/** The shape of the left column and the middle column, as React state. */
export interface WorkspaceState {
  /** Every file of the opened project, sorted by canonical project path. */
  files: ProjectFile[];
  /** The open tabs, in tab order (the order they were opened). */
  open: string[];
  /** The file the editor shows; always one of `open` (or `""` before the seed settles). */
  active: string;
  /** The tree's selection, which may be a directory. */
  selected: string;
  /** The directories whose children are hidden. */
  collapsed: string[];
  /** The files whose buffer differs from the project's copy. */
  dirty: string[];
}

export interface Workspace {
  state: WorkspaceState;
  /** The visible tree rows, depth-first, with collapsed subtrees left out. */
  rows: TreeRow[];
  /** The text of `path`: the edited buffer when there is one, else the project's copy. */
  textOf: (path: string) => string;
  /** The entry the project names (`tessera.json` `app.entry`), or `null` plus why (no fallback). */
  entry: EntryResult;
  /** Selects a row: a file also becomes active and gets a tab; a directory only becomes selected. */
  selectRow: (row: TreeRow) => void;
  /** Opens or closes a directory row's children (`ArrowRight`/`ArrowLeft` in the tree). */
  toggleDir: (path: string) => void;
  /** Opens (or focuses) a file, adding it to the project when the path is new. */
  openFile: (file: ProjectFile) => void;
  /** Closes a tab; the next tab becomes active, and the active file stays active when it is not the one. */
  closeTab: (path: string) => void;
  /** Writes a buffer, marking the file dirty the first time it differs from the project's copy. */
  writeBuffer: (path: string, text: string) => void;
  /** Adds a project file (a gallery sample, an import): confined by the caller's `projectFile`. */
  addFile: (file: ProjectFile) => void;
  /**
   * Replaces a file's project text and forgets its buffer, then opens and focuses it.
   *
   * This is how a sample, a share link or a view's own "Load sample" arrives: the project copy is what
   * the file *is*, and the buffer is dropped so the editor's effect writes the new text. Without it, a
   * sample written into an already-open file would leave the old buffer in front of the new text.
   */
  putFile: (file: ProjectFile) => void;
  /** Replaces the whole project (an opened folder or a dropped zip). */
  setProject: (files: ProjectFile[]) => void;
}

/** The workspace's initial state: the seeded project, with its entry open and selected. */
export function initialWorkspace(files: ProjectFile[] = seedProject()): WorkspaceState {
  const entry = fileAt(files, SEED_ENTRY) ?? files.find((file) => file.kind === "source") ?? files[0];
  const path = entry ? entry.path : "";
  return {
    files,
    open: path ? [path] : [],
    active: path,
    selected: path,
    collapsed: [],
    dirty: [],
  };
}

/**
 * The workspace hook: state, the buffer map and the actions the three regions call.
 *
 * The seed is the render-time fallback rather than an effect, so the server-rendered chrome (the Node
 * gates render `<App/>` with `renderToStaticMarkup`) draws the same tree a browser does on its first
 * paint, and the layout is reviewable without a load path.
 */
export function useWorkspace(): Workspace {
  const [state, setState] = useState<WorkspaceState>(() => initialWorkspace());
  // The one mutable store: file path -> the text the editor currently holds. A ref, for the reason in
  // the header: a keystroke must not re-render the page.
  const buffers = useRef<Map<string, string>>(new Map());

  const textOf = useCallback(
    (path: string): string => {
      const buffered = buffers.current.get(path);
      if (buffered !== undefined) {
        return buffered;
      }
      return fileAt(state.files, path)?.text ?? "";
    },
    [state.files],
  );

  const rows = useMemo(() => treeRows(state.files, new Set(state.collapsed)), [state.files, state.collapsed]);
  const entry = useMemo(() => entryOf(state.files, textOf), [state.files, state.dirty, textOf]);

  const selectRow = useCallback((row: TreeRow) => {
    setState((previous) => {
      if (row.dir) {
        // A directory row is a disclosure, not a document: it becomes the selection and nothing else.
        // Opening its children is `toggleDir`, which a click and ArrowRight/ArrowLeft both drive.
        return { ...previous, selected: row.path };
      }
      if (!row.editable) {
        // A locked file (a font, an image, C++) is selectable -- it is part of the project -- but it
        // has no text to edit, so it does not become the editor's active file.
        return { ...previous, selected: row.path };
      }
      return {
        ...previous,
        selected: row.path,
        active: row.path,
        open: previous.open.includes(row.path) ? previous.open : [...previous.open, row.path],
      };
    });
  }, []);

  const toggleDir = useCallback((path: string) => {
    setState((previous) => ({
      ...previous,
      selected: path,
      collapsed: previous.collapsed.includes(path)
        ? previous.collapsed.filter((candidate) => candidate !== path)
        : [...previous.collapsed, path],
    }));
  }, []);

  const openFile = useCallback((file: ProjectFile) => {
    setState((previous) => {
      const known = fileAt(previous.files, file.path) !== null;
      return {
        ...previous,
        files: known ? previous.files : [...previous.files, file].sort(byPath),
        open: previous.open.includes(file.path) ? previous.open : [...previous.open, file.path],
        active: file.editable ? file.path : previous.active,
        selected: file.path,
      };
    });
  }, []);

  const addFile = openFile;

  const putFile = useCallback((file: ProjectFile) => {
    buffers.current.delete(file.path);
    setState((previous) => {
      const known = fileAt(previous.files, file.path) !== null;
      return {
        ...previous,
        files: known
          ? previous.files.map((candidate) => (candidate.path === file.path ? file : candidate))
          : [...previous.files, file].sort(byPath),
        open: previous.open.includes(file.path) ? previous.open : [...previous.open, file.path],
        active: file.editable ? file.path : previous.active,
        selected: file.path,
        dirty: previous.dirty.filter((candidate) => candidate !== file.path),
      };
    });
  }, []);

  const closeTab = useCallback((path: string) => {
    setState((previous) => {
      if (!previous.open.includes(path)) {
        return previous;
      }
      const open = previous.open.filter((candidate) => candidate !== path);
      const active = previous.active === path ? (open[open.length - 1] ?? "") : previous.active;
      const dirty = previous.dirty.filter((candidate) => candidate !== path);
      buffers.current.delete(path);
      return { ...previous, open, active, dirty, selected: active === "" ? previous.selected : active };
    });
  }, []);

  const writeBuffer = useCallback((path: string, text: string) => {
    buffers.current.set(path, text);
    setState((previous) => {
      const original = fileAt(previous.files, path);
      // The marker is *derived* from the buffer and the project's copy rather than toggled: typing
      // back to the original text clears it, and a second keystroke in an already-dirty file is one
      // state update that changes nothing, so React re-renders the page once per dirty transition.
      const dirty = original !== null && original.text !== text;
      const was = previous.dirty.includes(path);
      if (dirty === was) {
        return previous;
      }
      return {
        ...previous,
        dirty: dirty
          ? [...previous.dirty, path]
          : previous.dirty.filter((candidate) => candidate !== path),
      };
    });
  }, []);

  const setProject = useCallback((files: ProjectFile[]) => {
    buffers.current.clear();
    setState(initialWorkspace(files));
  }, []);

  return {
    state,
    rows,
    textOf,
    entry,
    selectRow,
    toggleDir,
    openFile,
    closeTab,
    writeBuffer,
    addFile,
    putFile,
    setProject,
  };
}

function byPath(left: ProjectFile, right: ProjectFile): number {
  return left.path < right.path ? -1 : left.path > right.path ? 1 : 0;
}
