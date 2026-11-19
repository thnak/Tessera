// SPDX-License-Identifier: MIT

// Opening a locked text file read-only (issue #391, owner decision 2026-11-17).
//
// A locked file (C++, a generated header) is listed at load and read only when it becomes the editor's
// active file, under the per-file cap. What comes back is stored as `viewText` on the project file, which
// the viewer, Save and Download never look at: they filter on `editable`, so the bytes read here cannot
// reach the engine or a write by construction.

import { useEffect } from "react";
import type { ProjectIo } from "./page-project-io";
import type { Workspace } from "./page-workspace";

/** Reads the active file when it is a locked text file that was not read yet, then asks the page to redraw the editor. */
export function useViewOnlyLoader(workspace: Workspace, io: ProjectIo, onLoaded: () => void): void {
  const { active, files } = workspace.state;
  const file = files.find((candidate) => candidate.path === active);
  const pending = file?.viewOnly === true && file.viewText === undefined && file.viewNote === undefined;
  const { setViewRead } = workspace;
  const { readView } = io;
  useEffect(() => {
    if (!pending) {
      return;
    }
    let cancelled = false;
    void readView(active).then((read) => {
      if (!cancelled) {
        setViewRead(active, read);
        onLoaded();
      }
    });
    return () => {
      cancelled = true;
    };
  }, [pending, active, readView, setViewRead, onLoaded]);
}
