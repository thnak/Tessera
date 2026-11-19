// SPDX-License-Identifier: MIT

// The page's half of the style commit round trip (issue #585): how a `source.patch` reaches the file
// buffers. The protocol client (`lib/override-client.ts`) decides WHAT to apply and in what order; this hook
// is the host that owns the buffers.
//
//   * The file the editor shows takes the edits as ONE CodeMirror transaction (one text-history entry, rule
//     D3), which the editor reports as a commit rather than a keystroke (`editor/editor-commit.ts`), so no
//     debounce is scheduled and a non-source file (a theme, a stylesheet) does not restart the app.
//   * Any other file is written to the workspace buffer directly. It has no live editor history to enter, and
//     it is marked dirty like any edited buffer, so Save writes it.
//
// The hook reads the workspace and the live-edit controller through refs: the session that calls the bridge
// is created once and outlives every render.

import { useRef } from "react";
import { commitToEditor } from "./editor/editor-commit";
import type { LiveEdit } from "./page-live";
import type { Workspace } from "./page-workspace";
import type { CommitBridge } from "./lib/override-client";
import { applyChanges } from "./lib/utf8-edits";

export function useCommitBridge(workspace: Workspace, live: () => LiveEdit | null): CommitBridge {
  const workspaceRef = useRef(workspace);
  workspaceRef.current = workspace;
  const liveRef = useRef(live);
  liveRef.current = live;
  const bridge = useRef<CommitBridge | null>(null);
  if (bridge.current === null) {
    bridge.current = {
      text: (path) => workspaceRef.current.textOf(path),
      applyEdits: (path, changes, label) => {
        const current = workspaceRef.current;
        const shown = (globalThis as { __tesseraEditor?: { path: string } }).__tesseraEditor?.path;
        if (shown === path) {
          const text = commitToEditor(changes, label);
          if (text !== null) {
            current.writeBuffer(path, text);
            return;
          }
        }
        current.writeBuffer(path, applyChanges(current.textOf(path), changes));
      },
      applyNow: () => liveRef.current()?.applyNow() ?? Promise.resolve(),
    };
  }
  return bridge.current;
}
