// SPDX-License-Identifier: MIT

// The two page actions that turn something into the open project or a link, split out of `App.tsx` (the
// 500-line limit): a chosen sample becomes a project file, and Share encodes the entry and the toolbar into a
// link and copies it. The welcome view, the Open Sample quick pick and the toolbar's Share all end here.

import { useCallback, type MutableRefObject } from "react";
import type { LoadedSample } from "./lib/gallery";
import {
  DEFAULT_FILE,
  linkProblemMessage,
  shareStateOf,
  shareToLocation,
  type ShareState,
} from "./lib/preferences";
import { projectFile, type ProjectFile } from "./lib/project";
import { sampleProjectFiles } from "./page-boot";
import type { ProjectIo } from "./page-project-io";
import type { PageStateSetters } from "./page-state";
import type { Workspace } from "./page-workspace";
import type { ControlValues } from "./regions/StudioChrome";

export function useSampleAndShare({
  workspace,
  projectIo,
  state_,
  controls,
  entryPath,
  textOfRef,
  setRevision,
  sessions,
}: {
  workspace: Workspace;
  projectIo: ProjectIo;
  state_: PageStateSetters;
  controls: ControlValues;
  entryPath: string | null;
  textOfRef: MutableRefObject<(path: string) => string>;
  setRevision: React.Dispatch<React.SetStateAction<number>>;
  sessions: Record<string, { onSample?: (path: string) => void } | undefined>;
}) {
  const onSample = useCallback(
    (sample: LoadedSample) => {
      // A gallery sample becomes a project file at its own path, opened and selected: the tree shows where
      // it came from, and Run compiles it under the name the sample was shared under.
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

  const onShare = useCallback(async () => {
    state_.setShareStatus("encoding...");
    const shareState: ShareState = shareStateOf(
      entryPath === null ? "" : textOfRef.current(entryPath),
      controls,
      entryPath === null ? DEFAULT_FILE : entryPath,
    );
    try {
      const url = await shareToLocation(shareState);
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(url);
        state_.setShareStatus("link copied");
      } else {
        // No clipboard permission (or no secure context). The address bar already carries the link, and
        // saying so is better than a button that silently does nothing.
        state_.setShareStatus("link is in the address bar");
      }
    } catch (error) {
      state_.setShareStatus("");
      state_.setLinkNote(linkProblemMessage(error));
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [controls]);

  return { onSample, onShare };
}
