// SPDX-License-Identifier: MIT

// The studio page: four regions, three views, one editor, one toolbar (ADR-054 decision 3; the page itself
// is issue #392's merge, converted by issue #422).
//
// **What React renders, and what it does not.** This component renders the four regions
// `designer-studio-v1.md` section 3 names -- studio chrome, workspace pane, preview well, status line -- and
// nothing else. The engine draws exactly one thing, the preview canvas, and the page remains its host. React
// never renders the previewed app, never runs project code, and never renders a document a Wasm module
// produced: the diagnostics list, the scene snapshot, the state cells and the canvas are childless leaves a
// *session* fills (`src/views/*-session.ts`). A preview crash therefore loses a pane, not the editor.
//
// **Why the sessions live in a ref and not in state.** A session owns a Wasm module instance, a WebGPU
// device and an animation-frame callback. None of that may be created by a render or destroyed by a
// re-render, so each is created once against the committed DOM nodes and never re-created. The page's React
// state (`page-state.ts`) is exactly what a visitor changes by hand: the view, the toolbar, the gallery
// being open, the share status and the build id. That is the whole state model, and it is why a toolbar move
// never touches the canvas' GPU surface.
//
// **Laziness** is the merge's central constraint and it survives verbatim: the Diagnostics module is the
// only one on the first-paint path; Scene's is imported the first time that tab is shown; Canvas' module is
// imported on its first open and its WebGPU device is acquired on the first Run there. A visitor who only
// reads diagnostics pays for exactly what the P1 page cost, and a browser without WebGPU keeps Diagnostics
// and Scene and sees the named unsupported state in Canvas only
// (docs/design/reference-equantic-ui.md section 7.5).
//
// **The editor is one textarea for all three views.** That is the merge's product claim: the same source is
// compiled, laid out and painted without being copied between pages.

import { useCallback, useEffect, useRef, useState } from "react";
import type { LoadedSample } from "./lib/gallery";
import {
  DEFAULT_FILE,
  initialState,
  linkProblemMessage,
  rememberLocally,
  shareStateOf,
  shareToLocation,
  type ShareState,
} from "./lib/preferences";
import {
  DEFAULT_VIEW,
  resolveView,
  viewOf,
  writeViewToUrl,
  type ViewDefinition,
  type ViewSession,
} from "./lib/views";
import { STARTUP_SAMPLE } from "./page-defaults";
import { setStatusText, usePageState, usePaneRefs } from "./page-state";
import { CanvasPane } from "./regions/CanvasPane";
import { GalleryRegion } from "./regions/GalleryRegion";
import { EnvironmentToolbar, PageHeader, type ControlValues } from "./regions/StudioChrome";
import { StatusLine } from "./regions/StatusLine";
import { DiagnosticsPane, EditorPane, ScenePane } from "./regions/ViewPanes";
import { createCanvasSession } from "./views/canvas-session";
import { createDiagnosticsSession, SAMPLE as DIAGNOSTICS_SAMPLE } from "./views/diagnostics-session";
import { createSceneSession } from "./views/scene-session";

export function App() {
  const [view, setView] = useState<ViewDefinition["id"]>(() =>
    resolveView(globalThis.location?.search, DEFAULT_VIEW),
  );
  const [state, state_] = usePageState();
  const refs = usePaneRefs();

  // The three sessions, and which ones have been started. Refs, because a session is an identity: a
  // re-render must not rebuild a Wasm module instance or a WebGPU device.
  const sessions = useRef<Partial<Record<ViewDefinition["id"], ViewSession>>>({});
  const started = useRef<Partial<Record<ViewDefinition["id"], boolean>>>({});
  // The source path a share link names. A ref: the sessions read it at Run time, not at render time.
  const fileRef = useRef(DEFAULT_FILE);
  // The view the sessions' readiness callbacks are talking about (they are not re-created per view).
  const viewRef = useRef(view);
  viewRef.current = view;
  // The toolbar is settled once the fragment (or storage) has been decoded. The sessions wait for it, so a
  // `?view=scene#p5=<390x844>` link lays the scene out once, at 390x844.
  const [toolbarSettled, setToolbarSettled] = useState(false);

  const currentView = viewOf(view);

  // ---- the toolbar, as the two layout-aware sessions read it --------------------------------------
  const environment = useCallback(
    () => ({ viewport: state.controls.viewport, dpr: state.controls.dpr }),
    [state.controls],
  );
  const environmentOf = useCallback(() => {
    const [width, height] = state.controls.viewport.split("x").map(Number);
    return { width, height, dpr: Number(state.controls.dpr) };
  }, [state.controls]);
  // The fold posture (issue #408) is a layout input like the viewport, so both sessions read it the
  // same way they read the size and the DPR. A `""` value is "no fold", which is what every page
  // shows until a visitor picks a posture.
  const currentFoldPosture = useCallback(
    () => state.controls.foldPosture ?? "",
    [state.controls],
  );
  const currentFile = useCallback(() => fileRef.current, []);

  // ---- the shared chrome mounts before the first view is shown (issue #392's ordering rule) ---------
  //
  // A broken link still gets a usable page: the page's own sample is loaded and the banner says what was
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
        });
        if (refs.source.current) {
          refs.source.current.value = loaded.source === "" ? STARTUP_SAMPLE : loaded.source;
        }
        fileRef.current = loaded.file || DEFAULT_FILE;
        state_.setControls({
          viewport: `${loaded.width || 320}x${loaded.height || 240}`,
          dpr: String(loaded.dpr ?? 1),
          theme: String(loaded.theme ?? "system"),
          foldPosture: String(loaded.foldPosture ?? ""),
        });
      } catch (error) {
        if (refs.source.current) {
          refs.source.current.value = STARTUP_SAMPLE;
        }
        fileRef.current = DEFAULT_FILE;
        state_.setControls({
          viewport: "320x240",
          dpr: "1",
          theme: "system",
          foldPosture: "",
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
      .catch(() => {
        state_.setBuildId("build: local (no build-id.txt)");
        state_.setBuildIdTitle("The publish job writes build-id.txt; a local checkout has none.");
      });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ---- the sessions, created once against the committed nodes -------------------------------------
  useEffect(() => {
    if (!toolbarSettled || !refs.source.current) {
      return;
    }
    const source = refs.source.current;
    const isCurrent = (id: ViewDefinition["id"]): boolean => viewRef.current === id;
    const pane = refs;

    if (!sessions.current.diagnostics && pane.diagStatus.current && pane.diagList.current && pane.diagEmpty.current) {
      const status = pane.diagStatus.current;
      sessions.current.diagnostics = createDiagnosticsSession({
        elements: { source, status, list: pane.diagList.current, emptyState: pane.diagEmpty.current },
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        onReady: (ready) => {
          if (isCurrent("diagnostics")) {
            state_.setRunEnabled(ready);
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
          source,
          status,
          nodes: pane.nodes.current,
          emptyState: pane.sceneEmpty.current,
          editorHint: pane.editorHint.current,
          script: pane.script.current,
          inputStatus,
          cells: pane.cells.current,
          runInput: pane.runInput.current,
        },
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        setInputStatus: (text, kind) => setStatusText(inputStatus, text, kind ?? null),
        environment,
        foldPosture: currentFoldPosture,
        file: currentFile,
        onReady: (ready) => {
          if (isCurrent("scene")) {
            state_.setRunEnabled(ready);
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
      pane.statDpr.current
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
        },
        source,
        setStatus: (text, kind) => setStatusText(status, text, kind ?? null),
        environment: environmentOf,
        foldPosture: currentFoldPosture,
        file: currentFile,
        onReady: (ready) => {
          if (isCurrent("canvas")) {
            state_.setRunEnabled(ready);
          }
        },
      });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [toolbarSettled, environment, environmentOf, currentFoldPosture, currentFile]);

  // ---- show the view: mount its module on first open, and tell every session which one is visible ----
  useEffect(() => {
    for (const [id, candidate] of Object.entries(sessions.current)) {
      candidate?.setVisible?.(id === view);
    }
    const session = sessions.current[view];
    // A view that is already mounted can be run; one whose module is still loading cannot, and its own
    // `onReady` enables the button when it is.
    state_.setRunEnabled(Boolean(session) && started.current[view] === true);
    if (!session || started.current[view]) {
      return;
    }
    started.current[view] = true;
    void session.load().catch((error: unknown) => {
      // A view module that fails to import is that view's problem: it is reported in that pane's own status
      // line, and the other views keep working. The console carries the detail because a pane whose module
      // never loaded may have no status node to write into.
      setStatusText(
        view === "diagnostics"
          ? refs.diagStatus.current
          : view === "scene"
            ? refs.sceneStatus.current
            : refs.canvasStatus.current,
        "view unavailable",
        "warning",
      );
      console.error(`the ${view} view failed to load:`, error);
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [view, toolbarSettled]);

  // ---- the shared buttons and the editor's keyboard shortcut ----------------------------------------
  const runActiveView = useCallback(() => {
    const session = sessions.current[viewRef.current];
    if (session && typeof session.run === "function") {
      void session.run();
    }
  }, []);

  const loadSampleForActiveView = useCallback(() => {
    const session = sessions.current[viewRef.current];
    if (session?.loadSample) {
      session.loadSample();
      return;
    }
    // A view that defines no sample of its own still gets a usable editor rather than a dead button.
    if (refs.source.current) {
      refs.source.current.value = DIAGNOSTICS_SAMPLE;
      refs.source.current.focus();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const onEditorKeyDown = useCallback(
    (event: React.KeyboardEvent<HTMLTextAreaElement>) => {
      if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        runActiveView();
      }
    },
    [runActiveView],
  );

  const onSelectView = useCallback((id: ViewDefinition["id"]) => {
    setView(id);
    // `replace`, not `push`: a tab is not a navigation step a visitor wants to walk back through.
    writeViewToUrl(id);
  }, []);

  // ---- the toolbar ---------------------------------------------------------------------------------
  const onControlChange = useCallback(
    (name: keyof ControlValues, value: string) => {
      state_.setControls((previous) => {
        const next = { ...previous, [name]: value };
        rememberLocally(next);
        return next;
      });
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  // Every mounted view hears about a toolbar move, not only the visible one: a Scene tab that is hidden when
  // the toolbar changes would otherwise show bounds from a viewport the toolbar no longer names the moment
  // it is opened. The first run is skipped -- it is the fragment decode, not a visitor's change.
  const firstControls = useRef(true);
  useEffect(() => {
    if (firstControls.current) {
      firstControls.current = false;
      return;
    }
    for (const session of Object.values(sessions.current)) {
      session?.onEnvironmentChange?.();
    }
  }, [state.controls]);

  const onSample = useCallback(
    (sample: LoadedSample) => {
      fileRef.current = sample.path;
      if (refs.source.current) {
        refs.source.current.value = sample.text;
        refs.source.current.focus();
      }
      state_.setGalleryOpen(false);
      for (const session of Object.values(sessions.current)) {
        session?.onSample?.(sample.path);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  const onShare = useCallback(async () => {
    state_.setShareStatus("encoding...");
    const shareState: ShareState = shareStateOf(
      refs.source.current?.value ?? "",
      state.controls,
      fileRef.current,
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
  }, [state.controls]);

  return (
    <>
      <PageHeader
        view={currentView}
        buildId={state.buildId}
        buildIdTitle={state.buildIdTitle}
        onSelect={onSelectView}
        buildIdRef={refs.buildId}
      />

      <EnvironmentToolbar
        controls={state.controls}
        onChange={onControlChange}
        onToggleGallery={() => state_.setGalleryOpen((open) => !open)}
        galleryOpen={state.galleryOpen}
        onShare={() => void onShare()}
        shareStatus={state.shareStatus}
        shareStatusRef={refs.shareStatus}
      />

      <GalleryRegion open={state.galleryOpen} onPick={onSample} />

      <p className="link-note" id="link-note" role="alert" hidden={state.linkNote === null}>
        {state.linkNote}
      </p>

      <main className="layout">
        <EditorPane
          sourceRef={refs.source}
          editorHintRef={refs.editorHint}
          onRun={runActiveView}
          onLoadSample={loadSampleForActiveView}
          runDisabled={!state.runEnabled}
          onKeyDown={onEditorKeyDown}
        />

        <DiagnosticsPane
          view={currentView}
          statusRef={refs.diagStatus}
          listRef={refs.diagList}
          emptyRef={refs.diagEmpty}
        />

        <ScenePane
          view={currentView}
          statusRef={refs.sceneStatus}
          nodesRef={refs.nodes}
          emptyRef={refs.sceneEmpty}
          scriptRef={refs.script}
          inputStatusRef={refs.inputStatus}
          cellsRef={refs.cells}
          runInputRef={refs.runInput}
        />

        <CanvasPane
          view={currentView}
          statusRef={refs.canvasStatus}
          unsupportedRef={refs.canvasUnsupported}
          unsupportedBodyRef={refs.canvasUnsupportedBody}
          frameRef={refs.canvasFrame}
          canvasRef={refs.canvas}
          failedRef={refs.canvasFailed}
          failedBodyRef={refs.canvasFailedBody}
          frameStatsRef={refs.canvasStats}
          statFrameRef={refs.statFrame}
          statDrawsRef={refs.statDraws}
          statVerticesRef={refs.statVertices}
          statNodesRef={refs.statNodes}
          statDprRef={refs.statDpr}
        />
      </main>

      <StatusLine />
    </>
  );
}
