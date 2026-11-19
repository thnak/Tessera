// SPDX-License-Identifier: MIT

// The page's side of the Problems panel (owner decision, studio focus pass): the store the diagnostics session
// fills, the click-to-jump that opens the file and selects the span, and the `diagnostics.autoOpen` preference.
// Split out of `App.tsx` (the 500-line limit). The panel itself is `regions/ProblemsPanel.tsx`.

import { useCallback, useEffect, useRef, useState } from "react";
import { editorGotoLine } from "./editor/editor-target";
import type { Workbench } from "./dock/use-workbench";
import { createProblemsStore, type Problem, type ProblemsStore } from "./lib/problems";
import { AUTO_OPEN_KEY } from "./page-panels";
import type { PageRefs } from "./page-state";
import type { Workspace } from "./page-workspace";

export interface ProblemsHandle {
  store: ProblemsStore;
  /** Opens the problem's file and selects its span in the editor. */
  jump: (problem: Problem) => void;
}

function autoOpenEnabled(): boolean {
  try {
    return globalThis.localStorage?.getItem(AUTO_OPEN_KEY) === "1";
  } catch {
    return false;
  }
}

/** The store, created once; the sessions need it before the jump (which needs the tab handler) can exist. */
export function useProblemsStore(): ProblemsStore {
  return useState(createProblemsStore)[0];
}

export function useProblems({
  store,
  workbench,
  workspace,
  refs,
  onSelectTab,
}: {
  store: ProblemsStore;
  workbench: Workbench;
  workspace: Workspace;
  refs: PageRefs;
  onSelectTab: (path: string) => void;
}): ProblemsHandle {
  const [pending, setPending] = useState<Problem | null>(null);
  const active = workspace.state.active;
  const files = workspace.state.files;

  // A test and automation seam, like `__tesseraEditor` and `__tesseraWorkbench`: read the store, publish an
  // array as if a compile had produced it, and count the listeners (a hidden panel must not be one).
  useEffect(() => {
    const holder = globalThis as { __tesseraProblems?: ProblemsStore };
    holder.__tesseraProblems = store;
    return () => {
      delete holder.__tesseraProblems;
    };
  }, [store]);

  // A closed project has no problems; the status bar goes back to "not run yet".
  useEffect(() => {
    if (files.length === 0) {
      store.reset();
    }
  }, [files.length, store]);

  // `diagnostics.autoOpen` (default off, like VS Code's Problems): the first error of a clean state opens a
  // collapsed panel area.
  const commands = workbench.commands;
  useEffect(() => {
    let had = store.get().errors > 0;
    return store.subscribe(() => {
      const now = store.get().errors > 0;
      if (now && !had && autoOpenEnabled()) {
        commands.revealPanelArea();
      }
      had = now;
    });
  }, [store, commands]);

  const jump = useCallback(
    (problem: Problem) => {
      if (problem.file !== "" && problem.file !== active && files.some((file) => file.path === problem.file)) {
        onSelectTab(problem.file);
      }
      setPending(problem);
    },
    [active, files, onSelectTab],
  );

  // The editor swaps its document in an effect of the same commit as the tab change, and a parent's effect runs
  // after its children's, so by now `source.path` is the new file. One animation frame of grace covers a slower
  // commit; after that the jump is dropped rather than left to fire on some later, unrelated render.
  const retried = useRef<Problem | null>(null);
  useEffect(() => {
    if (pending === null) {
      return;
    }
    const attempt = (): boolean => {
      const editor = refs.source.current;
      if (editor === null || (pending.file !== "" && editor.path !== pending.file)) {
        return false;
      }
      if (pending.offset !== null) {
        editor.reveal(pending.offset, pending.length);
      } else if (pending.line > 0) {
        editorGotoLine(pending.line);
      } else {
        editor.focus();
      }
      return true;
    };
    if (attempt()) {
      setPending(null);
      return;
    }
    if (retried.current === pending) {
      setPending(null);
      return;
    }
    retried.current = pending;
    const frame = requestAnimationFrame(() => {
      attempt();
      setPending(null);
    });
    return () => cancelAnimationFrame(frame);
  }, [pending, active, refs.source]);

  return { store, jump };
}
