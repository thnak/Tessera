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
  findConflicts,
  loadDirectory,
  loadZip,
  readText,
  writeFiles,
} from "../project-io.js";
import { PathError } from "../project-paths.js";
import { writeZip, ZipError } from "../studio-zip.js";
import { projectFile, type ProjectFile } from "./lib/project";
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
}

interface DirectoryPickerWindow {
  showDirectoryPicker?: (options: { mode: "readwrite" }) => Promise<FileSystemDirectoryHandle>;
}

/** The sentence for a refusal. A `code` is branched on; the message is what a person reads. */
function sentenceOf(error: unknown): string {
  if (error instanceof ProjectIoError || error instanceof PathError || error instanceof ZipError) {
    return error.message;
  }
  return error instanceof Error ? error.message : String(error);
}

function toProjectFiles(
  entries: readonly { path: string; text: string }[],
  context: object,
): ProjectFile[] {
  return entries
    .map((entry) => projectFile(entry.path, entry.text, context))
    .sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
}

export interface ProjectIo {
  state: ProjectIoState;
  /** Counts completed loads; the page restarts the preview when it changes. */
  epoch: number;
  folderSupported: boolean;
  openFolder: () => void;
  openZip: (file: File) => void;
  save: () => void;
  resolveConflicts: (choice: "keep-mine" | "take-disk" | "cancel") => void;
  download: () => void;
}

export function useProjectIo(workspace: Workspace): ProjectIo {
  const [state, setState] = useState<ProjectIoState>({
    source: "sample",
    name: "",
    note: "",
    busy: false,
    conflicts: [],
  });
  const [epoch, setEpoch] = useState(0);
  const root = useRef<FileSystemDirectoryHandle | null>(null);
  const baseline = useRef<Map<string, number>>(new Map());
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
    if (workspaceRef.current.state.dirty.length === 0) {
      return true;
    }
    return globalThis.confirm?.("Opening another project discards the unsaved changes. Continue?") ?? true;
  }, []);

  const adopt = useCallback(
    (
      source: Source,
      name: string,
      project: { entries: { path: string; text: string }[]; context: object },
      handle: FileSystemDirectoryHandle | null,
      times: Map<string, number>,
    ) => {
      workspaceRef.current.setProject(toProjectFiles(project.entries, project.context));
      root.current = handle;
      baseline.current = times;
      setState({ source, name, note: "", busy: false, conflicts: [] });
      setEpoch((value) => value + 1);
    },
    [],
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
      if (!confirmReplace()) {
        return;
      }
      say({ busy: true, note: "reading the folder..." });
      try {
        const project = await loadDirectory(handle);
        adopt("folder", handle.name, project, handle, project.baseline);
      } catch (error) {
        say({ busy: false, note: sentenceOf(error) });
      }
    })();
  }, [adopt, confirmReplace, say]);

  const openZip = useCallback(
    (file: File) => {
      if (!confirmReplace()) {
        return;
      }
      void (async () => {
        say({ busy: true, note: "reading the archive..." });
        try {
          const project = await loadZip(new Uint8Array(await file.arrayBuffer()));
          adopt("zip", file.name.replace(/\.zip$/i, ""), project, null, new Map());
        } catch (error) {
          say({ busy: false, note: sentenceOf(error) });
        }
      })();
    },
    [adopt, confirmReplace, say],
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
    async (files: { path: string; text: string; kind: string }[]) => {
      const handle = root.current;
      if (handle === null) {
        return;
      }
      const written = await writeFiles(handle, files);
      for (const [path, time] of written) {
        baseline.current.set(path, time);
      }
      workspaceRef.current.markSaved(new Map(files.map((file) => [file.path, file.text])));
      say({
        busy: false,
        conflicts: [],
        note: files.length === 1 ? `saved ${files[0].path}` : `saved ${files.length} files`,
      });
    },
    [say],
  );

  const save = useCallback(() => {
    if (root.current === null) {
      say({ note: "No folder is open to save into. Use Download project, or open a folder." });
      return;
    }
    const files = pending();
    if (files.length === 0) {
      say({ note: "nothing to save" });
      return;
    }
    void (async () => {
      say({ busy: true, note: "saving..." });
      try {
        const conflicts = await findConflicts(
          root.current,
          files.map((file) => file.path),
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
        await write(files);
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
            if (rest.length > 0) {
              await write(rest);
            }
            say({ busy: false, conflicts: [], note: "took the version on disk for the changed file(s)" });
            setEpoch((value) => value + 1);
            return;
          }
          await write(files);
        } catch (error) {
          say({ busy: false, note: sentenceOf(error) });
        }
      })();
    },
    [pending, say, state.conflicts, write],
  );

  const download = useCallback(() => {
    const current = workspaceRef.current;
    const encoder = new TextEncoder();
    // Only accepted text files travel: a locked file was never read, so there are no bytes to export.
    const entries = current.state.files
      .filter((file) => file.editable)
      .map((file) => ({ path: file.path, bytes: encoder.encode(current.textOf(file.path)) }));
    const blob = new Blob([writeZip(entries) as BlobPart], { type: "application/zip" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `${state.name || "project"}.zip`;
    link.click();
    URL.revokeObjectURL(url);
    say({ note: `downloaded ${entries.length} file(s); locked files are not included` });
  }, [say, state.name]);

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

  return { state, epoch, folderSupported, openFolder, openZip, save, resolveConflicts, download };
}
