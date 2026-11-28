// SPDX-License-Identifier: MIT

// The page's project load and save (issue #391, D-S4): open a folder, open a dropped or chosen zip,
// Save (Ctrl/Cmd+S) back to the opened folder, and Download project as a zip.
//
// What decides *what a project is* lives in `project-io.js` (confinement, limits, the locked rule) and
// is tested in Node; this hook only drives it from the browser and turns its refusals into one visible
// sentence. Nothing here reads a file the project does not accept, and nothing writes outside the opened
// root.
//
// **Save is explicit and never overwrites silently.** Before any write the opened files' `lastModified`
// is compared with the value recorded at load; a file that changed on disk is offered back as a
// conflict (keep mine, take disk, cancel) and nothing is written until the person picks. There is no
// autosave (the owner's decision of 2026-11-16, #391).

import { useCallback, useEffect, useRef, useState } from "react";
import {
  ProjectIoError,
  deleteFiles,
  ensureWritable,
  findConflicts,
  loadDirectory,
  loadZip,
  readText,
  writeFiles,
} from "../project-io.js";
import {
  applicableDraft,
  buildDraft,
  clearDraft,
  loadDraft,
  projectKey,
  saveDraft,
} from "../drafts.js";
import { useRecentProjects, type RecentProject } from "./page-recent-projects";
import { staleGenerated } from "../generated-staleness.js";
import { PathError } from "../project-paths.js";
import { PackageError, writePackage } from "../project-package.js";
import { ZipError } from "../studio-zip.js";
import { projectFile, type ProjectFile } from "./lib/project";
import { missingSentence, sampleEntries, sampleProject } from "./lib/sample-folder";
import type { Workspace } from "./page-workspace";

type Source = "sample" | "folder" | "zip";

export interface ProjectIoState {
  source: Source;
  /** The opened folder's or archive's name, for the header and the download's file name. */
  name: string;
  /** One sentence about the last action (or its refusal); empty when there is nothing to say. */
  note: string;
  busy: boolean;
  /** The files whose copy on disk changed since load, while the person has not chosen yet. */
  conflicts: string[];
  /** A stored draft of unsaved edits for this project, offered back; `null` when there is none. */
  draft: { files: number; savedAt: number } | null;
  /**
   * Generated files whose source is newer than they are (advisory: "may be out of date", never a fact;
   * `generated-staleness.js`). Empty when nothing is flagged.
   */
  stale: readonly string[];
  /** The sample entries of a picked folder that holds several, waiting for `openChoice`; empty otherwise. */
  choices: readonly string[];
}

/** A loaded folder or zip before it becomes the project (it may be a sample with several entries). */
interface PickedTree {
  source: Source;
  name: string;
  project: { entries: LoadedEntry[]; context: object };
  handle: FileSystemDirectoryHandle | null;
  baseline: Map<string, number>;
}

interface DirectoryPickerWindow {
  showDirectoryPicker?: (options: { mode: "readwrite" }) => Promise<FileSystemDirectoryHandle>;
}

/** The sentence for a refusal. A `code` is branched on; the message is what a person reads. */
function sentenceOf(error: unknown): string {
  if (error instanceof ProjectIoError || error instanceof PathError || error instanceof ZipError || error instanceof PackageError) {
    return error.message;
  }
  return error instanceof Error ? error.message : String(error);
}

/** What opening a locked text file read; the shape `readViewOnly` in `project-io.js` returns. */
export type ViewRead = { text: string } | { note: string };

interface LoadedEntry {
  path: string;
  text: string;
  lastModified?: number;
  readView?: () => Promise<ViewRead>;
}

function toProjectFiles(entries: readonly LoadedEntry[], context: object): ProjectFile[] {
  return entries
    .map((entry) => ({ ...projectFile(entry.path, entry.text, context), modified: entry.lastModified ?? 0 }))
    .sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
}

export interface ProjectIo {
  state: ProjectIoState;
  /** Counts completed loads; the page restarts the preview when it changes. */
  epoch: number;
  folderSupported: boolean;
  /** The folders opened before that this browser still remembers, newest first (at most 10). */
  recents: readonly RecentProject[];
  /** `true` once the recent list was read. */
  recentsLoaded: boolean;
  /** Opens one of `state.choices` (a sample entry of the picked folder) as the project. */
  openChoice: (entry: string) => void;
  openFolder: () => void;
  /** Reopens `recents[index]`; the click that calls it is the gesture a permission prompt needs. */
  reopenRecent: (index: number) => void;
  /** Clears the list of recent projects (the files on disk are not touched). */
  forgetRecents: () => void;
  openZip: (file: File) => void;
  save: () => void;
  resolveConflicts: (choice: "keep-mine" | "take-disk" | "cancel") => void;
  download: () => Promise<void>;
  /** Reads a locked text file for the read-only editor; resolves to why there is no text when it has none. */
  readView: (path: string) => Promise<ViewRead>;
  /** Applies the stored draft to the opened project (skipping files that changed since), or drops it. */
  restoreDraft: () => void;
  discardDraft: () => void;
  /** File operations on the tree; each says what it did (or why not) in the note. */
  newFile: () => void;
  renameFile: () => void;
  deleteFile: () => void;
}

export function useProjectIo(workspace: Workspace): ProjectIo {
  const [state, setState] = useState<ProjectIoState>({
    source: "sample",
    name: "",
    note: "",
    busy: false,
    conflicts: [],
    draft: null,
    stale: [],
    choices: [],
  });
  const [epoch, setEpoch] = useState(0);
  const recent = useRecentProjects();
  const { recents } = recent;
  const root = useRef<FileSystemDirectoryHandle | null>(null);
  const baseline = useRef<Map<string, number>>(new Map());
  // Lazy readers of the locked text files of the opened project, by confined path. A file is read only when
  // it is opened; the reader holds the file handle's `File`, never a copy of its bytes.
  const viewReaders = useRef<Map<string, () => Promise<ViewRead>>>(new Map());
  const workspaceRef = useRef(workspace);
  workspaceRef.current = workspace;

  const folderSupported =
    typeof globalThis !== "undefined" &&
    typeof (globalThis as DirectoryPickerWindow).showDirectoryPicker === "function";

  const say = useCallback((patch: Partial<ProjectIoState>) => {
    setState((previous) => ({ ...previous, ...patch }));
  }, []);

  /** Asks before throwing away unsaved edits; `true` when it is fine to replace the project. */
  const confirmReplace = useCallback((): boolean => {
    const { dirty, removed } = workspaceRef.current.state;
    if (dirty.length === 0 && removed.length === 0) {
      return true;
    }
    return globalThis.confirm?.("Opening another project discards the unsaved changes. Continue?") ?? true;
  }, []);

  // A stored draft of unsaved edits for the project just opened. It is offered, never applied silently, and
  // while an offer is open the periodic save leaves the stored draft alone so it cannot be overwritten by
  // the first keystroke.
  const pendingDraft = useRef<{ key: string; draft: ReturnType<typeof buildDraft> } | null>(null);
  const draftStored = useRef(false);

  const offerDraft = useCallback(
    async (source: Source, name: string, files: readonly { path: string; text: string }[]) => {
      const key = projectKey(source, name);
      const draft = await loadDraft(key);
      if (draft === null) {
        return;
      }
      const usable = applicableDraft(draft, files);
      const count = usable.edits.length + usable.removed.length;
      if (count === 0) {
        // Nothing in it fits the project as opened (every file changed since): it is stale, so it goes.
        void clearDraft(key);
        return;
      }
      pendingDraft.current = { key, draft };
      setState((previous) => ({ ...previous, draft: { files: count, savedAt: draft.savedAt } }));
    },
    [],
  );

  const adopt = useCallback(
    (
      source: Source,
      name: string,
      project: { entries: LoadedEntry[]; context: object },
      handle: FileSystemDirectoryHandle | null,
      times: Map<string, number>,
    ) => {
      workspaceRef.current.setProject(toProjectFiles(project.entries, project.context));
      viewReaders.current = new Map(
        project.entries.flatMap((entry) => (entry.readView ? [[entry.path, entry.readView] as const] : [])),
      );
      root.current = handle;
      baseline.current = times;
      if (handle !== null) {
        recent.remember(handle);
      }
      setState({
        source,
        name,
        note: "",
        busy: false,
        conflicts: [],
        draft: null,
        choices: [],
        stale: staleGenerated(
          project.entries.map((entry) => ({
            path: entry.path,
            kind: entry.path.endsWith(".tsx") || entry.path.endsWith(".jsx") ? "source" : "other",
            text: entry.text,
            modified: entry.lastModified ?? 0,
          })),
        ),
      });
      setEpoch((value) => value + 1);
      pendingDraft.current = null;
      void offerDraft(source, name, toProjectFiles(project.entries, project.context));
    },
    [],
  );

  // A picked folder or zip that is a sample, not a project (`lib/sample-folder.ts`): kept until an entry is
  // chosen, because a repository root or `samples/gallery` holds several.
  const pendingTree = useRef<PickedTree | null>(null);

  const openChoice = useCallback(
    (entry: string) => {
      const tree = pendingTree.current;
      if (tree === null) {
        return;
      }
      const sample = sampleProject(tree.project.entries, entry);
      const copied = new Set(["tessera.json", ...sample.companions.map((companion) => companion.path)]);
      const onDisk = new Map(tree.project.entries.map((item) => [item.path, item]));
      const entries = sample.files.map((file) => ({
        ...file,
        lastModified: copied.has(file.path) ? 0 : (onDisk.get(file.path)?.lastModified ?? 0),
      }));
      const baseline = new Map([...tree.baseline].filter(([path]) => !copied.has(path) && onDisk.has(path)));
      const stem = entry.slice(entry.lastIndexOf("/") + 1).replace(/\.tsx$/, "");
      adopt(tree.source, `${tree.name}-${stem}`, { entries, context: tree.project.context }, tree.handle, baseline);
      // The project opens on its first source file, which may be a copied `Button.tsx`; show the chosen entry.
      const text = entries.find((file) => file.path === entry)?.text ?? "";
      workspaceRef.current.openFile(projectFile(entry, text, tree.project.context));
      const from = sample.companions.map((companion) => companion.from).join(", ");
      say({
        note:
          sample.missing.length > 0
            ? missingSentence(sample.missing)
            : from === ""
              ? `Opened ${entry}.`
              : `Opened ${entry} with ${from} copied next to it (Save writes a copy only after you edit it).`,
      });
    },
    [adopt, say],
  );

  const adoptTree = useCallback(
    (tree: PickedTree) => {
      const entries = sampleEntries(tree.project.entries);
      pendingTree.current = entries.length === 0 ? null : tree;
      if (entries.length === 0) {
        adopt(tree.source, tree.name, tree.project, tree.handle, tree.baseline);
      } else if (entries.length === 1) {
        openChoice(entries[0]);
      } else {
        say({ busy: false, choices: entries, note: `${tree.name} holds ${entries.length} samples: choose one to open.` });
      }
    },
    [adopt, openChoice, say],
  );

  /** Loads a picked or remembered folder; a folder that no longer opens is forgotten. */
  const loadFrom = useCallback(
    async (handle: FileSystemDirectoryHandle) => {
      if (!confirmReplace()) {
        return;
      }
      say({ busy: true, note: "reading the folder..." });
      try {
        const project = await loadDirectory(handle);
        adoptTree({ source: "folder", name: handle.name, project, handle, baseline: project.baseline });
      } catch (error) {
        say({ busy: false, note: sentenceOf(error) });
        if (error instanceof DOMException && error.name === "NotFoundError") {
          say({ note: `${handle.name} no longer exists, so it was removed from the recent projects.` });
          recent.forget(handle);
        }
      }
    },
    [adoptTree, confirmReplace, recent, say],
  );

  const openFolder = useCallback(() => {
    const picker = (globalThis as DirectoryPickerWindow).showDirectoryPicker;
    if (picker === undefined) {
      say({ note: "This browser cannot open a folder. Drop a zip of the project instead." });
      return;
    }
    void (async () => {
      let handle: FileSystemDirectoryHandle;
      try {
        handle = await picker({ mode: "readwrite" });
      } catch (error) {
        if (!(error instanceof DOMException && error.name === "AbortError")) {
          say({ note: sentenceOf(error) });
        }
        return;
      }
      await loadFrom(handle);
    })();
  }, [loadFrom, say]);

  const forgetRecents = useCallback(() => {
    recent.forgetAll();
    say({ note: "Forgot the recent projects. The folders themselves were not touched." });
  }, [recent, say]);

  const reopenRecent = useCallback((index: number) => {
    const handle = recent.handleAt(index);
    if (handle === undefined) {
      return;
    }
    void (async () => {
      try {
        // The click that got here is the user gesture a permission prompt needs.
        await ensureWritable(handle);
      } catch (error) {
        say({ note: sentenceOf(error) });
        return;
      }
      await loadFrom(handle);
    })();
  }, [loadFrom, say]);

  const openZip = useCallback(
    (file: File) => {
      if (!confirmReplace()) {
        return;
      }
      void (async () => {
        say({ busy: true, note: "reading the archive..." });
        try {
          const project = await loadZip(new Uint8Array(await file.arrayBuffer()));
          adoptTree({ source: "zip", name: file.name.replace(/\.zip$/i, ""), project, handle: null, baseline: new Map() });
        } catch (error) {
          say({ busy: false, note: sentenceOf(error) });
        }
      })();
    },
    [adoptTree, confirmReplace, say],
  );

  /** The files to write: every dirty one, with the text the editor holds. */
  const pending = useCallback((): { path: string; text: string; kind: string }[] => {
    const current = workspaceRef.current;
    return current.state.dirty.flatMap((path) => {
      const file = current.state.files.find((candidate) => candidate.path === path);
      return file === undefined ? [] : [{ path, text: current.textOf(path), kind: file.kind }];
    });
  }, []);

  const write = useCallback(
    async (files: { path: string; text: string; kind: string }[], deletes: string[] = []) => {
      const handle = root.current;
      if (handle === null) {
        return;
      }
      const written = await writeFiles(handle, files);
      await deleteFiles(handle, deletes);
      for (const [path, time] of written) {
        baseline.current.set(path, time);
      }
      for (const path of deletes) {
        baseline.current.delete(path);
      }
      workspaceRef.current.markSaved(new Map(files.map((file) => [file.path, file.text])), deletes);
      const current = workspaceRef.current;
      const stale = staleGenerated(
        current.state.files.map((file) => ({
          path: file.path,
          kind: file.kind,
          text: current.textOf(file.path),
          modified: baseline.current.get(file.path) ?? file.modified ?? 0,
        })),
      );
      const parts = [
        files.length === 1 ? `saved ${files[0].path}` : files.length > 1 ? `saved ${files.length} files` : "",
        deletes.length > 0 ? `deleted ${deletes.length} file(s) from the folder` : "",
      ].filter((part) => part !== "");
      say({ busy: false, conflicts: [], note: parts.join("; "), stale });
    },
    [say],
  );

  const save = useCallback(() => {
    if (root.current === null) {
      say({ note: "No folder is open to save into. Use Download project, or open a folder." });
      return;
    }
    const files = pending();
    const deletes = [...workspaceRef.current.state.removed];
    if (files.length === 0 && deletes.length === 0) {
      say({ note: "nothing to save" });
      return;
    }
    void (async () => {
      say({ busy: true, note: "saving..." });
      try {
        const conflicts = await findConflicts(
          root.current,
          [...files.map((file) => file.path), ...deletes],
          baseline.current,
        );
        if (conflicts.length > 0) {
          say({
            busy: false,
            conflicts,
            note: `${conflicts.length} file(s) changed on disk since they were opened.`,
          });
          return;
        }
        await write(files, deletes);
      } catch (error) {
        say({ busy: false, note: sentenceOf(error) });
      }
    })();
  }, [pending, say, write]);

  const resolveConflicts = useCallback(
    (choice: "keep-mine" | "take-disk" | "cancel") => {
      const conflicted = new Set(state.conflicts);
      if (choice === "cancel") {
        say({ conflicts: [], note: "save cancelled; nothing was written" });
        return;
      }
      void (async () => {
        say({ busy: true });
        try {
          const files = pending();
          const deletes = [...workspaceRef.current.state.removed];
          if (choice === "take-disk") {
            for (const path of conflicted) {
              const disk = root.current === null ? null : await readText(root.current, path);
              const file = workspaceRef.current.state.files.find((candidate) => candidate.path === path);
              if (disk !== null && file !== undefined) {
                workspaceRef.current.putFile({ ...file, text: disk.text });
                baseline.current.set(path, disk.lastModified);
              }
            }
            const rest = files.filter((file) => !conflicted.has(file.path));
            const restDeletes = deletes.filter((path) => !conflicted.has(path));
            if (rest.length > 0 || restDeletes.length > 0) {
              await write(rest, restDeletes);
            }
            say({ busy: false, conflicts: [], note: "took the version on disk for the changed file(s)" });
            setEpoch((value) => value + 1);
            return;
          }
          await write(files, deletes);
        } catch (error) {
          say({ busy: false, note: sentenceOf(error) });
        }
      })();
    },
    [pending, say, state.conflicts, write],
  );

  const restoreDraft = useCallback(() => {
    const offer = pendingDraft.current;
    const current = workspaceRef.current;
    if (offer === null || offer.draft === null) {
      return;
    }
    const usable = applicableDraft(offer.draft, current.state.files);
    // A file the person has already started editing again is theirs: the draft does not overwrite it.
    const typing = new Set(current.state.dirty);
    let restored = 0;
    for (const edit of usable.edits) {
      if (typing.has(edit.path)) {
        usable.skipped.push(edit.path);
        continue;
      }
      if (edit.base === null) {
        const refusal = current.createFile(edit.path);
        if (refusal !== null) {
          usable.skipped.push(edit.path);
          continue;
        }
      }
      current.writeBuffer(edit.path, edit.text);
      restored += 1;
    }
    for (const path of usable.removed) {
      current.deleteFile(path);
      restored += 1;
    }
    pendingDraft.current = null;
    setState((previous) => ({
      ...previous,
      draft: null,
      note:
        usable.skipped.length === 0
          ? `restored ${restored} unsaved change(s)`
          : `restored ${restored}; skipped ${usable.skipped.join(", ")} (changed since the draft)`,
    }));
    setEpoch((value) => value + 1);
  }, []);

  const discardDraft = useCallback(() => {
    const offer = pendingDraft.current;
    pendingDraft.current = null;
    if (offer !== null) {
      void clearDraft(offer.key);
    }
    setState((previous) => ({ ...previous, draft: null, note: "draft discarded" }));
  }, []);

  // The sample project can have a draft too: it is the project a first visit starts in.
  useEffect(() => {
    void offerDraft("sample", "", workspaceRef.current.state.files);
  }, [offerDraft]);

  // Unsaved edits are kept in this browser every couple of seconds while there are any, and when the tab
  // is hidden, so a reload or a crash loses at most the last moments. Saving to disk clears the draft.
  useEffect(() => {
    const key = projectKey(state.source, state.name);
    const flush = (): void => {
      if (pendingDraft.current !== null) {
        return;
      }
      const current = workspaceRef.current;
      const draft = buildDraft(current.state.files, current.textOf, {
        dirty: current.state.dirty,
        added: current.state.added,
        removed: current.state.removed,
        removedBase: current.removedTexts(),
      });
      if (draft !== null) {
        draftStored.current = true;
        void saveDraft(key, draft);
      } else if (draftStored.current) {
        draftStored.current = false;
        void clearDraft(key);
      }
    };
    const timer = globalThis.setInterval(flush, 2000);
    const onHide = (): void => {
      if (document.hidden) {
        flush();
      }
    };
    document.addEventListener("visibilitychange", onHide);
    return () => {
      globalThis.clearInterval(timer);
      document.removeEventListener("visibilitychange", onHide);
    };
  }, [state.source, state.name]);

  const readView = useCallback(async (path: string): Promise<ViewRead> => {
    const reader = viewReaders.current.get(path);
    return reader === undefined ? { note: "unreadable" } : reader();
  }, []);

  const download = useCallback(async () => {
    const current = workspaceRef.current;
    const encoder = new TextEncoder();
    // The package keeps each file's time (an edited file is stamped now), so a reopened package can tell a
    // stale generated header. The accepted text files travel with their edits; a locked *text* file is read
    // the same way the view-only editor reads it, and a binary or over-cap one stays out.
    const now = Date.now();
    const entries: { path: string; bytes: Uint8Array; modified: number }[] = [];
    for (const file of current.state.files) {
      if (file.editable) {
        const modified = current.state.dirty.includes(file.path) ? now : (baseline.current.get(file.path) ?? file.modified ?? 0);
        entries.push({ path: file.path, bytes: encoder.encode(current.textOf(file.path)), modified });
      } else if (file.viewOnly) {
        const read = await readView(file.path);
        if ("text" in read) {
          entries.push({ path: file.path, bytes: encoder.encode(read.text), modified: file.modified ?? 0 });
        }
      }
    }
    const blob = new Blob([writePackage(entries) as BlobPart], { type: "application/zip" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${state.name || "project"}.zip`;
    link.click();
    URL.revokeObjectURL(url);
    say({ note: `downloaded ${entries.length} file(s) as a project package (binary locked files are not included)` });
  }, [readView, say, state.name]);

  const newFile = useCallback(() => {
    const current = workspaceRef.current;
    const selected = current.state.selected;
    const row = current.rows.find((candidate) => candidate.path === selected);
    const folder = row === undefined ? "" : row.dir ? row.path : row.path.split("/").slice(0, -1).join("/");
    const answer = globalThis.prompt?.("New file path, relative to the project", folder === "" ? "" : `${folder}/`);
    if (answer === null || answer === undefined || answer.trim() === "") {
      return;
    }
    const refusal = current.createFile(answer.trim());
    say({ note: refusal ?? `created ${answer.trim()}; it is written when you Save` });
  }, [say]);

  const selectedFile = useCallback(() => {
    const current = workspaceRef.current;
    return current.state.files.find((file) => file.path === current.state.selected) ?? null;
  }, []);

  /** The selected directory's path, or `null` when the selection is a file (or nothing). */
  const folderSelected = useCallback((): string | null => {
    const current = workspaceRef.current;
    const row = current.rows.find((candidate) => candidate.path === current.state.selected);
    return row !== undefined && row.dir ? row.path : null;
  }, []);

  const renameFile = useCallback(() => {
    const file = selectedFile();
    const dir = file === null ? folderSelected() : null;
    if (dir === null && (file === null || !file.editable)) {
      say({ note: "Select an editable file or a folder to rename." });
      return;
    }
    const from = dir ?? (file as { path: string }).path;
    const answer = globalThis.prompt?.(
      "New path. Imports in other files are not rewritten; fix them yourself.",
      from,
    );
    if (answer === null || answer === undefined || answer.trim() === "" || answer.trim() === from) {
      return;
    }
    const refusal =
      dir === null ? workspaceRef.current.renameFile(from, answer.trim()) : workspaceRef.current.renameDir(from, answer.trim());
    say({ note: refusal ?? `renamed to ${answer.trim()}; imports in other files were not rewritten` });
  }, [say, selectedFile, folderSelected]);

  const deleteFile = useCallback(() => {
    const file = selectedFile();
    const dir = file === null ? folderSelected() : null;
    if (dir !== null) {
      const where = root.current === null ? "from the project" : "from the project and, at Save, from the folder";
      if (!(globalThis.confirm?.(`Delete folder ${dir} and every file in it ${where}?`) ?? false)) {
        return;
      }
      const refusal = workspaceRef.current.deleteDir(dir);
      say({ note: refusal ?? `deleted folder ${dir}${root.current === null ? "" : "; the folder keeps its files until you Save"}` });
      return;
    }
    if (file === null || !file.editable) {
      say({ note: "Select an editable file to delete." });
      return;
    }
    const where = root.current === null ? "from the project" : "from the project and, at Save, from the folder";
    if (!(globalThis.confirm?.(`Delete ${file.path} ${where}?`) ?? false)) {
      return;
    }
    workspaceRef.current.deleteFile(file.path);
    say({ note: `deleted ${file.path}${root.current === null ? "" : "; the folder keeps it until you Save"}` });
  }, [say, selectedFile, folderSelected]);

  // No file watcher exists, so a change made by another editor is noticed when the tab regains focus.
  useEffect(() => {
    const onFocus = (): void => {
      const handle = root.current;
      if (handle === null || baseline.current.size === 0) {
        return;
      }
      void findConflicts(handle, [...baseline.current.keys()], baseline.current)
        .then((changed) => {
          if (changed.length > 0) {
            say({ note: `${changed.length} file(s) changed on disk since they were opened.` });
          }
        })
        .catch(() => undefined);
    };
    globalThis.addEventListener("focus", onFocus);
    return () => globalThis.removeEventListener("focus", onFocus);
  }, [say]);

  return {
    state,
    epoch,
    folderSupported,
    recents,
    recentsLoaded: recent.loaded,
    openChoice,
    openFolder,
    reopenRecent,
    forgetRecents,
    readView,
    openZip,
    save,
    resolveConflicts,
    download,
    restoreDraft,
    discardDraft,
    newFile,
    renameFile,
    deleteFile,
  };
}
