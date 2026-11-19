// SPDX-License-Identifier: MIT

// The page action that turns a chosen sample into the open project, split out of `App.tsx` (the 500-line
// limit). The welcome view and the Open Sample quick pick both end here.

import { useCallback } from "react";
import type { LoadedSample } from "./lib/gallery";
import { projectFile, type ProjectFile } from "./lib/project";
import { sampleProjectFiles } from "./page-boot";
import type { ProjectIo } from "./page-project-io";
import type { Workspace } from "./page-workspace";

export function useSample({
  workspace,
  projectIo,
  setRevision,
  sessions,
}: {
  workspace: Workspace;
  projectIo: ProjectIo;
  setRevision: React.Dispatch<React.SetStateAction<number>>;
  sessions: Record<string, { onSample?: (path: string) => void } | undefined>;
}) {
  const onSample = useCallback(
    (sample: LoadedSample) => {
      // A gallery sample becomes a project file at its own path, opened and selected: the tree shows where
      // it came from, and Run compiles it under the sample's own name.
      let file: ProjectFile | null = null;
      try {
        file = projectFile(sample.path, sample.text);
      } catch {
        // A manifest path that is not a project path is refused by `project-paths.js`; the page keeps the
        // seed project rather than inventing a path.
        file = null;
      }
      if (file !== null && workspace.state.files.length === 0) {
        // Nothing is open (the studio no longer boots on a demo project): the sample becomes a project.
        projectIo.openSample(sample.component, sampleProjectFiles(file.path, sample.text, sample.component));
      } else if (file !== null) {
        workspace.putFile(file);
        setRevision((value) => value + 1);
      }
      for (const session of Object.values(sessions)) {
        session?.onSample?.(sample.path);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [workspace.state.files],
  );

  return { onSample };
}
