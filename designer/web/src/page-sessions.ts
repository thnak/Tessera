// SPDX-License-Identifier: MIT

// The three preview sessions, created once against the committed DOM (issue #431 split this out of
// `App.tsx` when the workspace pushed the page past the source limit; the sessions themselves are #422's).
//
// **Why a ref and not state.** A session owns a Wasm module instance, a WebGPU device and an
// animation-frame callback. None of that may be created by a render or destroyed by a re-render, so each is
// created lazily in an effect against the nodes React has committed, and never re-created. What this hook
// returns is therefore a stable handle on three identities, not a value that changes.
//
// **What it does not own.** The refs, the workspace and the toolbar state belong to the page; this module
// only knows how to turn them into sessions, and it never reads the editor's text -- a session asks the
// page's `ProjectReader` at Run time (issue #431: Run compiles the project file, not whatever is on screen).

import { useEffect, useRef } from "react";
import type { ProjectReader, ViewDefinition, ViewSession } from "./lib/views";
import type { PageRefs, PageStateSetters } from "./page-state";
import { setStatusText } from "./page-state";
import type { SkinSafeArea } from "./views/canvas-session";
import { createCanvasSession } from "./views/canvas-session";
import { createDiagnosticsSession } from "./views/diagnostics-session";
import { createSceneSession } from "./views/scene-session";

/** What a session needs from the page: the committed nodes, the toolbar, and the source it compiles. */
export interface SessionDeps {
  refs: PageRefs;
  setters: Pick<PageStateSetters, "setRunEnabled">;
  /** The toolbar's current viewport and DPR, as layout inputs (Scene). */
  environment: () => { viewport: string; dpr: string };
  /** The toolbar's current `{width, height, dpr}` (Canvas). */
  environmentOf: () => { width: number; height: number; dpr: number };
  /** The fold posture (issue #408); `""` is no fold. */
  foldPosture: () => string;
  /**
   * The selected skin's safe area (issue #171's skins slice), or `null` for no skin.
   *
   * A reader, like the toolbar's other values: the session is created once and the picker can move
   * between two applies. The page derives it from the validated manifest, so the insets the engine
   * pads by are the ones the manifest declared and the frame draws.
   */
  safeArea: () => SkinSafeArea | null;
  /** The component the preview dropdown picked (`""` = the entry's first), and where the canvas reports the list. */
  component: () => string;
  onComponents: (names: string[], chosen: string) => void;
  /**
   * The workspace's compile target plus the author stylesheet it is styled with, read at Run time.
   *
   * The stylesheet travels in the snapshot (issue #390, through #431's `ProjectReader`), so a session asks
   * one question and gets the file to compile and the sheet to compile it against together.
   */
  source: ProjectReader;
  /** `true` while `id` is the view the preview column is showing (a readiness callback's guard). */
  isCurrent: (id: ViewDefinition["id"]) => boolean;
}

/** The three sessions, keyed by view. Read through `.current`; the object never changes identity. */
export interface Sessions {
  current: Partial<Record<ViewDefinition["id"], ViewSession>>;
  /** Which sessions have had `load()` called, so a second switch does not re-import a module. */
  started: { current: Partial<Record<ViewDefinition["id"], boolean>> };
}

/**
 * Creates the three sessions once, as soon as the chrome has settled and its nodes exist.
 *
 * `settled` gates creation on the fragment/storage decode: a `?view=scene#p5=<390x844>` link must lay the
 * scene out once, at 390x844, rather than first at the default rectangle and then again.
 */
export function useSessions(deps: SessionDeps, settled: boolean): Sessions {
  const sessions = useRef<Partial<Record<ViewDefinition["id"], ViewSession>>>({});
  const started = useRef<Partial<Record<ViewDefinition["id"], boolean>>>({});
  // The toolbar readers, read through a ref (issue #436). The effect below runs once, but `deps`'s
  // `environment`/`environmentOf`/`foldPosture` are `useCallback`s over the toolbar state, so they are
  // NEW functions after every control change. Capturing the first ones gave the Canvas session a
  // permanently stale viewport: the select moved, `onEnvironmentChange` fired, and the module was
  // handed 320x240 forever. The sessions therefore ask this ref, which always holds the newest deps.
  const latest = useRef(deps);
  latest.current = deps;

  useEffect(() => {
    const pane = deps.refs;
    const source = pane.source.current;
    if (!settled || source === null) {
      return;
    }

    if (!sessions.current.diagnostics && pane.diagStatus.current && pane.diagList.current && pane.diagEmpty.current) {
      const status = pane.diagStatus.current;
      sessions.current.diagnostics = createDiagnosticsSession({
        elements: {
          status,
          list: pane.diagList.current,
          emptyState: pane.diagEmpty.current,
          editor: source,
          statusCount: pane.statusDiagnostics.current,
        },
        source: deps.source,
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        onReady: (ready) => {
          if (deps.isCurrent("diagnostics")) {
            deps.setters.setRunEnabled(ready);
          }
        },
      });
    }

    if (
      !sessions.current.scene &&
      pane.sceneStatus.current &&
      pane.nodes.current &&
      pane.sceneEmpty.current &&
      pane.editorHint.current &&
      pane.script.current &&
      pane.inputStatus.current &&
      pane.cells.current
    ) {
      const status = pane.sceneStatus.current;
      const inputStatus = pane.inputStatus.current;
      sessions.current.scene = createSceneSession({
        elements: {
          status,
          nodes: pane.nodes.current,
          emptyState: pane.sceneEmpty.current,
          editorHint: pane.editorHint.current,
          script: pane.script.current,
          inputStatus,
          cells: pane.cells.current,
          runInput: pane.runInput.current,
        },
        source: deps.source,
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        setInputStatus: (text, kind) => setStatusText(inputStatus, text, kind ?? null),
        environment: () => latest.current.environment(),
        foldPosture: () => latest.current.foldPosture(),
        onReady: (ready) => {
          if (deps.isCurrent("scene")) {
            deps.setters.setRunEnabled(ready);
          }
        },
      });
    }

    if (
      !sessions.current.canvas &&
      pane.canvas.current &&
      pane.canvasStatus.current &&
      pane.canvasUnsupported.current &&
      pane.canvasUnsupportedBody.current &&
      pane.canvasFrame.current &&
      pane.canvasFailed.current &&
      pane.canvasFailedBody.current &&
      pane.canvasStats.current &&
      pane.statFrame.current &&
      pane.statDraws.current &&
      pane.statVertices.current &&
      pane.statNodes.current &&
      pane.statDpr.current &&
      pane.statFont.current &&
      pane.stateRow.current &&
      pane.statState.current &&
      pane.pickOverlay.current
    ) {
      const status = pane.canvasStatus.current;
      sessions.current.canvas = createCanvasSession({
        elements: {
          status,
          unsupported: pane.canvasUnsupported.current,
          unsupportedBody: pane.canvasUnsupportedBody.current,
          frame: pane.canvasFrame.current,
          // The engine's one surface. React rendered it once and will not render it again.
          canvas: pane.canvas.current,
          failed: pane.canvasFailed.current,
          failedBody: pane.canvasFailedBody.current,
          frameStats: pane.canvasStats.current,
          statFrame: pane.statFrame.current,
          statDraws: pane.statDraws.current,
          statVertices: pane.statVertices.current,
          statNodes: pane.statNodes.current,
          statDpr: pane.statDpr.current,
          statFont: pane.statFont.current,
          // The state row (#444): what the running evaluation's cells read after an apply.
          stateRow: pane.stateRow.current,
          statState: pane.statState.current,
          // Issue #171's picker: the overlay the outline is drawn into, and the header toggle.
          pickOverlay: pane.pickOverlay.current,
          pickToggle: pane.pickToggle.current,
          statusFrame: pane.statusFrame.current,
        },
        source: deps.source,
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        environment: () => latest.current.environmentOf(),
        foldPosture: () => latest.current.foldPosture(),
        safeArea: () => latest.current.safeArea(),
        component: () => latest.current.component(),
        onComponents: (names, chosen) => latest.current.onComponents(names, chosen),
        onReady: (ready) => {
          if (deps.isCurrent("canvas")) {
            deps.setters.setRunEnabled(ready);
          }
        },
      });
    }
    // One creation pass per settle: the deps are refs-and-callbacks, not values a render changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [settled]);

  return { current: sessions.current, started };
}
