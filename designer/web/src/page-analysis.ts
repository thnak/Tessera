// SPDX-License-Identifier: MIT

// The page's side of editor code intelligence (#984, #988): it attaches the project to the analysis hub, so F12,
// Shift+F12, hover, completion and F2 rename reach the workspace. A span is shown through the Problems jump (open
// the file, then select the span once the editor swapped its document), rename writes the workspace buffers and
// asks the page to reload and recompile as a replace does, and the references go to a quick pick. It also
// publishes `service.analysis.ready`, which disables the `go.*` commands once the worker is known to be missing.

import { useEffect, useRef } from "react";
import { analysisHub } from "./editor/analysis-hub";
import { lineColumnOf } from "./lib/project-problems";
import { referencePick } from "./lib/reference-pick";
import type { Problem } from "./lib/problems";
import type { Workbench } from "./page-commands";
import type { Workspace } from "./page-workspace";

export function useAnalysisHub({
  workspace,
  jump,
  onWritten,
  commands,
}: {
  workspace: Workspace;
  jump: (problem: Problem) => void;
  onWritten: (paths: readonly string[]) => void;
  commands: Workbench;
}): void {
  const latest = useRef({ workspace, jump, onWritten, commands });
  latest.current = { workspace, jump, onWritten, commands };

  useEffect(() => {
    const open = (target: { path: string; offset: number; length: number }): void => {
      const { line, column } = lineColumnOf(latest.current.workspace.textOf(target.path), target.offset);
      latest.current.jump({ key: 0, severity: "info", code: "", message: "", file: target.path, line, column, offset: target.offset, length: target.length });
    };
    const detach = analysisHub.attach({
      get files() {
        return latest.current.workspace.state.files;
      },
      textOf: (path) => latest.current.workspace.textOf(path),
      openAt: open,
      writeBuffers(writes) {
        for (const write of writes) {
          latest.current.workspace.writeBuffer(write.path, write.text);
        }
        latest.current.onWritten(writes.map((write) => write.path));
      },
      notify: (notice) => {
        latest.current.commands.notify(notice);
      },
      pickReference(rows) {
        referencePick.set(rows, (row) => open(row));
        latest.current.commands.palette.open("", "references");
      },
    });
    const publish = (): void => {
      latest.current.commands.registry.context.set({ "service.analysis.ready": analysisHub.ready() });
    };
    publish();
    const unsubscribe = analysisHub.subscribe(publish);
    return () => {
      unsubscribe();
      detach();
    };
  }, []);
}
