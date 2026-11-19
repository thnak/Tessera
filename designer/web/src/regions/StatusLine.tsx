// SPDX-License-Identifier: MIT

// The status bar, cut down to what a code editor's bar carries (owner feedback on #586; VS Code's is the
// reference): Problems on the left, the caret and selection, the file's language and the notification bell on
// the right. The build id, compile target, frame time and the "part of Tessera" paragraph are gone from it:
// the build and the description are in Help > About, the frame time is in the Canvas panel's own stats, the
// compile target is the Run menu's business and the tooltip of the language cell.
//
// **Which items appear, and in what order, is data**: `menus.json` `statusBar` lists widgets (`problems`,
// `caret`, `language`, `bell`), the registry-backed builder keeps only what is live, and this file only draws
// each one. An item that runs something runs a command id (`view.togglePanelArea`, `view.gotoLine`).
//
// **Two cells are written without React.** A compile and a keystroke must not re-render the page
// (ADR-054 decision 3), so the Problems count and the caret reach their `<span>`s through refs the
// diagnostics session and the editor write; React renders the shell and the language.
//
// The bar is the one emphasised band of the page (`--status-bg`, VS Code's coloured status bar); everything
// in it stays text on that band, so contrast is the token pair `studio_accent_contrast.mjs` checks.

import type { RefObject } from "react";
import type { Workbench } from "../page-commands";
import { NotificationBell } from "./Notifications";

/** The language the status bar shows for a project path (the editor's own language rule, by extension). */
export function languageOf(path: string): string {
  if (/\.tsx$/i.test(path)) return "TSX";
  if (/\.(ts|mts)$/i.test(path)) return "TypeScript";
  if (/\.(jsx|js|mjs)$/i.test(path)) return "JavaScript";
  if (/\.css$/i.test(path)) return "CSS";
  if (/\.json$/i.test(path)) return "JSON";
  if (/\.(hpp|cpp|h|cc)$/i.test(path)) return "C++";
  return "Plain Text";
}

export function StatusLine({
  workbench,
  diagnosticsRef,
  caretRef,
  activePath,
  entryPath,
  diagnosticsOpen,
  hidden,
}: {
  workbench: Workbench;
  /** The Problems count: written by the diagnostics session when it compiles. */
  diagnosticsRef: RefObject<HTMLSpanElement | null>;
  /** The caret cell: written by the editor. */
  caretRef: RefObject<HTMLSpanElement | null>;
  /** The file the editor shows, or `""` when nothing is open. */
  activePath: string;
  /** The canonical project path Run compiles, or `""`. */
  entryPath: string;
  diagnosticsOpen: boolean;
  /** `view.toggleStatusBar` and zen mode hide the bar; its cells keep their refs, so sessions still write them. */
  hidden?: boolean;
}) {
  const language = activePath === "" ? "" : languageOf(activePath);
  return (
    <footer className="page-footer" aria-label="Status" hidden={hidden}>
      <div className="status-line">
        {workbench.statusItems.map((item) => {
          switch (item.widget) {
            case "problems":
              return (
                <button
                  key={item.key}
                  type="button"
                  className="status-item status-toggle"
                  id="status-diagnostics-toggle"
                  data-command="view.togglePanelArea"
                  aria-pressed={diagnosticsOpen}
                  aria-label="Problems: toggle the panel area"
                  title="Problems. Show or hide the panel area (Ctrl+J)"
                  onClick={() => void workbench.run("view.togglePanelArea")}
                >
                  <span aria-hidden="true">{"⊗"}</span>{" "}
                  <span id="status-diagnostics" ref={diagnosticsRef}>
                    not run yet
                  </span>
                </button>
              );
            case "caret":
              return activePath === "" ? null : (
                <button
                  key={item.key}
                  type="button"
                  className="status-item status-end"
                  data-command="view.gotoLine"
                  title="Go to line (Ctrl+G)"
                  onClick={() => void workbench.run("view.gotoLine")}
                >
                  <span id="status-caret" ref={caretRef}>
                    Ln 1, Col 1
                  </span>
                </button>
              );
            case "language":
              return activePath === "" ? null : (
                <span
                  key={item.key}
                  className="status-item"
                  id="status-language"
                  title={`${activePath}${entryPath === "" ? "" : `. Run compiles ${entryPath}`}`}
                >
                  {language}
                </span>
              );
            case "bell":
              return (
                <span key={item.key} className="status-item status-bell">
                  <NotificationBell workbench={workbench} tabIndex={0} />
                </span>
              );
            default:
              return null;
          }
        })}
      </div>
    </footer>
  );
}
