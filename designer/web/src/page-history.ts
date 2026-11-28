// SPDX-License-Identifier: MIT

// The page's side of version history (#583): local snapshots of each file's text, a timeline of the active
// file, and restore.
//
// **Where snapshots come from.** A save (after the folder write, from `page-project-io.ts`), the text a Format
// Document is about to rewrite (`onBeforeFormat` in `editor/editor-target.ts`), and the text of a file about to be renamed. A
// restore records the working text it replaces. All of it goes through `snapshot-store.js` (IndexedDB, this
// browser only); storage that is missing or full is a sentence in the panel, never an exception.
//
// **Restore is an editor edit.** The panel never writes the buffer when the file is open in the editor: it
// dispatches the one minimal edit as a single transaction, so Edit > Undo (Ctrl+Z) returns the working text in
// one step. A file the editor does not show gets the buffer written instead and the page reloads the editor.

import { useCallback, useEffect, useMemo, useRef, useState, type MutableRefObject } from "react";
import { projectKey } from "../drafts.js";
import { emptyHistory, renamePath, renameTree } from "../snapshots.js";
import { createSnapshotStore, indexedDbKeyValue } from "../snapshot-store.js";
import { applyEditorEdit, onBeforeFormat } from "./editor/editor-target";
import { describeTimeline, diffRows, planRestore, previousVersion, type DiffLine, type TimelineRow, type Snapshot } from "./lib/history-model";
import type { ProjectIoState } from "./page-project-io";
import type { Workspace } from "./page-workspace";

// The JS modules are untyped (`checkJs` is off), so the shape is stated here once.
interface History {
  version: 1;
  next: number;
  entries: Snapshot[];
}
type Store = ReturnType<typeof createSnapshotStore>;

/** What `useProjectIo` calls; the history hook fills the ref in once it has the project key. */
export interface ProjectHistoryHooks {
  /** Files just written to the folder, with the text written. */
  saved(files: readonly { path: string; text: string }[]): void;
  /** A file (or a folder when `dir`) is about to be renamed; `texts` are the files' working texts. */
  renaming(from: string, to: string, dir: boolean, texts: ReadonlyMap<string, string>): void;
}

export interface HistoryHandle {
  /** The active file's timeline, newest first. */
  rows: readonly TimelineRow[];
  path: string;
  /** The selected version and its changed lines against the working text. */
  selected: string | null;
  diff: { rows: DiffLine[]; hidden: number } | null;
  notice: string;
  select(id: string | null): void;
  restore(id: string): void;
  /** Restore Previous Version (`history.restorePrevious`): the newest version that differs from the working text. */
  restorePrevious(): void;
  refresh(): void;
}

export interface HistoryDeps {
  workspace: Workspace;
  state: ProjectIoState;
  hooks: MutableRefObject<ProjectHistoryHooks | null>;
  /** Called with the path after a restore wrote the buffer instead of the editor, so the page reloads the editor. */
  onWritten: (paths: readonly string[]) => void;
  visible: boolean;
  /** Test seam: the key-value layer, the clock. */
  store?: Store;
  now?: () => number;
}

export function useHistory(deps: HistoryDeps): HistoryHandle {
  const depsRef = useRef(deps);
  depsRef.current = deps;
  const storeRef = useRef<Store | null>(null);
  const store = (): Store => {
    storeRef.current ??= depsRef.current.store ?? createSnapshotStore(indexedDbKeyValue());
    return storeRef.current;
  };
  const clock = (): number => (depsRef.current.now ?? Date.now)();
  const key = projectKey(deps.state.source, deps.state.name);
  const keyRef = useRef(key);
  keyRef.current = key;

  const [history, setHistory] = useState<History>(() => emptyHistory() as History);
  const [selected, setSelected] = useState<string | null>(null);
  const [notice, setNotice] = useState("");
  const [tick, setTick] = useState(0);

  const record = useCallback(async (path: string, text: string, reason: string): Promise<void> => {
    const outcome = await store().record(keyRef.current, { path, text, reason, at: clock() });
    if (!outcome.ok) {
      setNotice(outcome.reason === "quota" ? "Version history is full for this browser; older versions were not kept." : "Version history is unavailable in this browser.");
    }
    if (outcome.added) {
      setHistory(outcome.history as History);
    }
  }, []);

  useEffect(() => {
    let live = true;
    setSelected(null);
    void store()
      .load(key)
      .then((loaded) => live && setHistory(loaded as History));
    return () => {
      live = false;
    };
  }, [key, tick]);

  useEffect(() => {
    const hooks: ProjectHistoryHooks = {
      saved: (files) => void Promise.all(files.map((file) => record(file.path, file.text, "save"))),
      renaming: (from, to, dir, texts) => {
        void (async () => {
          for (const [path, text] of texts) {
            await record(path, text, "rename");
          }
          const loaded = await store().load(keyRef.current);
          const moved = (dir ? renameTree(loaded, from, to) : renamePath(loaded, from, to)) as History;
          if ((await store().save(keyRef.current, moved)).ok) {
            setHistory(moved);
          }
        })();
      },
    };
    deps.hooks.current = hooks;
    const stop = onBeforeFormat((path, text) => void record(path, text, "format"));
    return () => {
      stop();
      if (deps.hooks.current === hooks) {
        deps.hooks.current = null;
      }
    };
  }, [deps.hooks, record]);

  const path = deps.workspace.state.active;
  const text = deps.visible ? deps.workspace.textOf(path) : "";
  const rows = useMemo(
    () => (deps.visible ? describeTimeline(history, path, text, clock()) : []),
    // The clock is read when the rows are built; the panel refreshes on edits, saves and tab changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [history, path, text, deps.visible],
  );
  const snapshot = selected === null ? undefined : history.entries.find((entry: Snapshot) => entry.id === selected);
  const diff = useMemo(() => (snapshot === undefined ? null : diffRows(snapshot.text, text)), [snapshot, text]);

  const restoreSnapshot = useCallback(
    async (target: Snapshot): Promise<void> => {
      const { workspace } = depsRef.current;
      const current = workspace.textOf(target.path);
      const plan = planRestore(current, target.text);
      if (plan.noop) {
        setNotice("That version is the current text.");
        return;
      }
      if (plan.keep !== null) {
        await record(target.path, plan.keep.text, plan.keep.reason);
      }
      if (!applyEditorEdit(target.path, plan.edit)) {
        workspace.writeBuffer(target.path, target.text);
        depsRef.current.onWritten([target.path]);
      }
      setNotice(`Restored ${target.path}. Undo (Ctrl+Z) brings the previous text back.`);
    },
    [record],
  );

  return {
    rows,
    path,
    selected,
    diff,
    notice,
    select: setSelected,
    restore: (id) => {
      const target = history.entries.find((entry: Snapshot) => entry.id === id);
      if (target !== undefined) {
        void restoreSnapshot(target);
      }
    },
    restorePrevious: () => {
      const { workspace } = depsRef.current;
      const target = previousVersion(history, workspace.state.active, workspace.textOf(workspace.state.active));
      if (target === undefined) {
        setNotice("No earlier version of this file differs from the current text.");
        return;
      }
      void restoreSnapshot(target);
    },
    refresh: () => setTick((value) => value + 1),
  };
}
