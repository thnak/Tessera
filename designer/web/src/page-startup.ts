// SPDX-License-Identifier: MIT

// How the page starts, split out of `App.tsx` (the 500-line limit): decode the share link or stored toolbar into
// the page's controls, and read the deployed build id. Both run once, on mount, and both degrade to a usable page
// (a broken link says so in a banner; a missing `build-id.txt` is a local checkout).

import { useEffect } from "react";
import { DEFAULT_FILE, initialState, linkProblemMessage } from "./lib/preferences";
import { entryOf } from "./lib/project-snapshot";
import { STARTUP_SAMPLE } from "./page-defaults";
import { hasShareLink, sampleProjectFiles } from "./page-boot";
import type { PageRefs, PageStateSetters } from "./page-state";
import type { ProjectIo } from "./page-project-io";
import type { Workspace } from "./page-workspace";

export function useStartup({
  refs,
  workspace,
  projectIo,
  state_,
  setRevision,
  setToolbarSettled,
}: {
  refs: PageRefs;
  workspace: Workspace;
  projectIo: ProjectIo;
  state_: PageStateSetters;
  setRevision: React.Dispatch<React.SetStateAction<number>>;
  setToolbarSettled: (settled: boolean) => void;
}): void {
  // ---- the shared chrome mounts before the first view is shown (issue #392's ordering rule) ---------
  //
  // A broken link still gets a usable page: the seeded project is loaded and the banner says what was
  // wrong with the link. The alternative -- an empty editor -- reads as a broken page.
  useEffect(() => {
    void (async () => {
      try {
        const loaded = await initialState({
          source: STARTUP_SAMPLE,
          file: DEFAULT_FILE,
          width: 320,
          height: 240,
          dpr: 1,
          theme: "system",
          foldPosture: "",
          skin: "",
          profile: "",
        });
        if (loaded.source !== "" && refs.source.current !== null) {
          // A link's source replaces the entry file's text: a share link is one document, and the entry is
          // the file it compiles under.
          const named = entryOf(workspace.state.files, workspace.textOf).path;
          const entry = workspace.state.files.find((file) => file.path === named) ?? null;
          if (entry !== null) {
            workspace.putFile({ ...entry, text: loaded.source });
            setRevision((value) => value + 1);
          } else if (workspace.state.files.length === 0 && hasShareLink()) {
            // Nothing is open: the link's one document becomes a one-file project.
            projectIo.openSample("shared", sampleProjectFiles("ui/Shared.tsx", loaded.source, "Shared"));
          }
        }
        // A share link carries one source document (the link envelope is frozen at `v,s,w,h,d,t,f`), so
        // it replaces the entry's text above; sharing a whole project waits for cloud storage.
        state_.setControls({
          viewport: `${loaded.width || 320}x${loaded.height || 240}`,
          dpr: String(loaded.dpr ?? 1),
          theme: String(loaded.theme ?? "system"),
          foldPosture: String(loaded.foldPosture ?? ""),
          skin: String(loaded.skin ?? ""),
          profile: String(loaded.profile ?? ""),
        });
      } catch (error) {
        // The seeded project is what the page shows when the link is broken (#431).
        state_.setControls({
          viewport: "320x240",
          dpr: "1",
          theme: "system",
          foldPosture: "",
          skin: "",
          profile: "",
        });
        state_.setLinkNote(linkProblemMessage(error));
      } finally {
        setToolbarSettled(true);
      }
    })();
    // Runs once: this is the page's initial state, not a reaction to a render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ---- the build id (D-S5: the deployed URL serves the current build) -------------------------------
  useEffect(() => {
    void fetch("./build-id.txt")
      .then((response) => (response.ok ? response.text() : Promise.reject(new Error("absent"))))
      .then((text) => state_.setBuildId("build: " + (text.trim() || "unknown")))
      .catch(() => state_.setBuildId("build: local (no build-id.txt)"));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
}
