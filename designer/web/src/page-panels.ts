// SPDX-License-Identifier: MIT

// Panel visibility to sessions (issue #516, W5 #560). Before the dock, the page had one `view` and an effect
// that told every session whether it was the visible one. Now several panels can be visible at once (Canvas and
// Scene side by side), and a panel can be open but hidden (a background tab, a collapsed group), so the answer
// comes from the layout model's visible set (`Workbench.visible`):
//
//   * a visible view panel's session is shown (`setVisible(true)`), its module is imported on the first show
//     and the first automatic apply runs (#444): laziness is unchanged;
//   * a hidden one is told `setVisible(false)`, which is what keeps a hidden Canvas from drawing;
//   * Diagnostics is also started while hidden, because it is the first-paint module and the status bar's
//     Diagnostics count (the only thing a collapsed panel area shows) is written by its session.
//
// `view` stays: it is the most recently shown view panel, which is what `?view=`, the Run button's readiness
// and the live edit's "update the visible pane first" ordering mean now.

import { useEffect, useRef, type MutableRefObject } from "react";
import type { Workbench } from "./dock/use-workbench";
import { VIEWS, viewFromSearch, writeViewToUrl, type ViewDefinition } from "./lib/views";
import type { LiveEdit } from "./page-live";
import type { Sessions } from "./page-sessions";
import { setStatusText, type PageRefs } from "./page-state";

type ViewId = ViewDefinition["id"];
const VIEW_IDS: readonly ViewId[] = VIEWS.map((view) => view.id);

export interface PanelVisibilityDeps {
  visible: ReadonlySet<string>;
  sessions: Sessions;
  live: MutableRefObject<LiveEdit | null>;
  refs: PageRefs;
  toolbarSettled: boolean;
  /** The comma-joined open view panels: a changed list means a session may have been created or disposed. */
  present: string;
  viewRef: MutableRefObject<ViewId>;
  setView: (view: ViewId) => void;
  setRunEnabled: (enabled: boolean) => void;
}

export function usePanelVisibility(deps: PanelVisibilityDeps): void {
  const previous = useRef<ReadonlySet<string>>(new Set());
  const first = useRef(true);
  const { visible, sessions, live, refs, toolbarSettled, present, viewRef, setView, setRunEnabled } = deps;

  useEffect(() => {
    const gained = VIEW_IDS.find((id) => visible.has(id) && !previous.current.has(id));
    previous.current = visible;
    let current = viewRef.current;
    if (gained !== undefined) {
      current = gained;
    } else if (!visible.has(current)) {
      current = VIEW_IDS.find((id) => visible.has(id)) ?? current;
    }
    if (current !== viewRef.current) {
      viewRef.current = current;
      setView(current);
      // `replace`, not `push`; and not at boot, where the layout's own choice is not a navigation.
      if (!first.current) {
        writeViewToUrl(current);
      }
    }
    first.current = false;

    for (const id of VIEW_IDS) {
      sessions.current[id]?.setVisible?.(visible.has(id));
    }
    // A view that is already mounted can be restarted; one whose module is still loading cannot, and its own
    // `onReady` enables the button when it is.
    setRunEnabled(Boolean(sessions.current[current]) && sessions.started.current[current] === true);

    for (const id of VIEW_IDS) {
      const session = sessions.current[id];
      if (!session || sessions.started.current[id] || !(visible.has(id) || id === "diagnostics")) {
        continue;
      }
      sessions.started.current[id] = true;
      void session
        .load()
        // The first automatic apply (issue #444): `applyNow()` is the debounce's action with the wait removed,
        // and it is what acquires the WebGPU device, lazily, here and never on page load.
        .then(() => live.current?.applyNow())
        .catch((error: unknown) => {
          // A view module that fails to import is that panel's problem: it is reported in its own status
          // line and the others keep working.
          const status = id === "diagnostics" ? refs.diagStatus : id === "scene" ? refs.sceneStatus : refs.canvasStatus;
          setStatusText(status.current, "view unavailable", "warning");
          console.error(`the ${id} view failed to load:`, error);
        });
    }
    // The refs and setters are stable; the inputs are the visible set, the settle and the open panels.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible, toolbarSettled, present]);
}

/** `?view=scene` still opens on that view: the named panel is opened (or revealed) once, at boot. */
export function useViewFromUrl(workbench: Workbench): void {
  useEffect(() => {
    const named = viewFromSearch(globalThis.location?.search);
    if (named !== null) {
      workbench.dispatch({ kind: "open", panelId: named });
    }
    // Runs once: it is the page's initial state, not a reaction to a render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
}

export const AUTO_OPEN_KEY = "tessera.studio.diagnostics.autoOpen";

function autoOpenEnabled(): boolean {
  try {
    return globalThis.localStorage?.getItem(AUTO_OPEN_KEY) === "1";
  } catch {
    return false;
  }
}

/**
 * `diagnostics.autoOpen` (default off, like VS Code's Problems): when a compile first reports a problem, a
 * collapsed panel area opens. The count lives in a status-bar cell the Diagnostics session writes, so this
 * watches that cell rather than asking the session.
 */
export function useDiagnosticsAutoOpen(workbench: Workbench, countRef: { current: HTMLElement | null }): void {
  const commands = workbench.commands;
  useEffect(() => {
    const cell = countRef.current;
    if (cell === null) {
      return;
    }
    const hasProblems = (): boolean => /^[1-9]\d* error\(s\)|^\d+ error\(s\), [1-9]/.test(cell.textContent ?? "");
    let had = hasProblems();
    const observer = new MutationObserver(() => {
      const now = hasProblems();
      if (now && !had && autoOpenEnabled()) {
        commands.revealPanelArea();
      }
      had = now;
    });
    observer.observe(cell, { childList: true, characterData: true, subtree: true });
    return () => observer.disconnect();
  }, [commands, countRef]);
}
