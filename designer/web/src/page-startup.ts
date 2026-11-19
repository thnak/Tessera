// SPDX-License-Identifier: MIT

// How the page starts, split out of `App.tsx` (the 500-line limit): read the stored toolbar into the page's
// controls, and read the deployed build id. Both run once, on mount, and both degrade to a usable page (a
// missing `build-id.txt` is a local checkout).

import { useEffect } from "react";
import { DEFAULT_FILE, initialState } from "./lib/preferences";
import type { PageStateSetters } from "./page-state";

export function useStartup({
  state_,
  setToolbarSettled,
}: {
  state_: PageStateSetters;
  setToolbarSettled: (settled: boolean) => void;
}): void {
  // ---- the shared chrome mounts before the first view is shown (issue #392's ordering rule) ---------
  useEffect(() => {
    const loaded = initialState({
      source: "",
      file: DEFAULT_FILE,
      width: 320,
      height: 240,
      dpr: 1,
      theme: "system",
      foldPosture: "",
      skin: "",
      profile: "",
    });
    state_.setControls({
      viewport: `${loaded.width || 320}x${loaded.height || 240}`,
      dpr: String(loaded.dpr ?? 1),
      theme: String(loaded.theme ?? "system"),
      foldPosture: String(loaded.foldPosture ?? ""),
      skin: String(loaded.skin ?? ""),
      profile: String(loaded.profile ?? ""),
    });
    setToolbarSettled(true);
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
