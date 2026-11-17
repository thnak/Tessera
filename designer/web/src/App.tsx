// SPDX-License-Identifier: MIT

// The studio: **one workspace, three columns** (issue #431), built on the shell #392 merged and #422
// converted.
//
// Before #431 the page was three top-level tabs -- Diagnostics, Scene, Canvas -- which is a preview demo:
// opening Canvas replaced the editor. The workspace is now the layout the issue asks for, always:
//
//   +--------------+---------------------------+---------------------------+
//   | project tree |  code (tabs per open file)|  preview                  |
//   |              |                           |  [scene] [canvas] [diag]  |
//   +--------------+---------------------------+---------------------------+
//   | status line: build id, compile target, diagnostics count, frame      |
//   +----------------------------------------------------------------------+
//
// **No view switch changes this shape.** The scene/canvas/diagnostics switch is a tablist *inside* the
// preview column (`regions/PreviewColumn.tsx`); the editor's tabs are a tablist over the open buffers
// (`regions/CodeRegion.tsx`); the page itself is not a tablist any more. `?view=` still resolves and is
// still written on a switch, so a link made before this issue opens on the view it names, and a `#p5=`
// fragment still decodes to the same state.
//
// **What React renders, and what it does not.** React renders the five regions, the tree, the tabs, the
// status cells and every box and status line; the engine still draws exactly one thing, the preview canvas,
// and every subtree a Wasm module produces is a childless leaf a *session* fills
// (`src/views/*-session.ts`, ADR-054 decision 3). A preview crash loses a pane, not the editor.
//
// **Why the sessions live in a ref and not in state.** A session owns a Wasm module instance, a WebGPU
// device and an animation-frame callback. None of that may be created by a render or destroyed by a
// re-render, so each is created once against the committed DOM nodes and never re-created
// (`page-sessions.ts`). The page's React state is exactly what a visitor changes by hand: the workspace
// (`page-workspace.ts`), the toolbar, the gallery being open, the share status and the build id.
//
// **Laziness** survives verbatim: the Diagnostics module is the only one on the first-paint path; Scene's is
// imported the first time that pane is shown; Canvas' is imported on its first open and its WebGPU device is
// acquired on the first Run there.

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
import { ENTRY_FILE, projectFile, type ProjectFile } from "./lib/project";
import {
  DEFAULT_VIEW,
  resolveView,
  viewOf,
  writeViewToUrl,
  type SourceSnapshot,
  type ViewDefinition,
} from "./lib/views";
import { STARTUP_SAMPLE } from "./page-defaults";
import { setStatusText, usePageState, usePaneRefs } from "./page-state";
import { useSessions } from "./page-sessions";
import { useWorkspace } from "./page-workspace";
import { CodeRegion } from "./regions/CodeRegion";
import { GalleryRegion } from "./regions/GalleryRegion";
import { PreviewColumn } from "./regions/PreviewColumn";
import { ProjectTree } from "./regions/ProjectTree";
import { PageHeader, type ControlValues } from "./regions/StudioChrome";
import { StatusLine } from "./regions/StatusLine";

export function App() {
  const [view, setView] = useState<ViewDefinition["id"]>(() =>
    resolveView(globalThis.location?.search, DEFAULT_VIEW),
  );
  const [state, state_] = usePageState();
  const refs = usePaneRefs();
  const workspace = useWorkspace();
  // The view the sessions' readiness callbacks are talking about (they are not re-created per view).
  const viewRef = useRef(view);
  viewRef.current = view;
  // The toolbar is settled once the fragment (or storage) has been decoded. The sessions wait for it, so a
  // `?view=scene#p5=<390x844>` link lays the scene out once, at 390x844.
  const [toolbarSettled, setToolbarSettled] = useState(false);
  // Bumped when the page itself replaces a buffer (a tab switch, a share link, a gallery pick), so the
  // editor's effect can tell a page write from a keystroke.
  const [revision, setRevision] = useState(0);

  const currentView = viewOf(view);
  const activeText = workspace.textOf(workspace.state.active);

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

  // ---- what Run compiles (issue #431) --------------------------------------------------------------
  //
  // The editor holds one project file at a time; Run compiles the workspace's *compile target* (the active
  // source file, else the project entry). The sessions read this at Run time, through a ref, so a keystroke
  // never re-creates a session and a stylesheet tab never becomes the compiled source.
  const target = workspace.target;
  const targetRef = useRef<ProjectFile | null>(target);
  targetRef.current = target;
  const shownRef = useRef(workspace.state.active);
  shownRef.current = workspace.state.active;
  const textOfRef = useRef(workspace.textOf);
  textOfRef.current = workspace.textOf;
  const currentSource = useCallback((): SourceSnapshot => {
    const file = targetRef.current;
    if (file === null) {
      return { text: "", file: DEFAULT_FILE, shown: false };
    }
    // The project's text, not the textarea's: when the editor is showing another file, the buffer that
    // belongs to `file` is the one the compiler must see.
    return {
      text: textOfRef.current(file.path),
      file: file.path,
      shown: shownRef.current === file.path,
    };
  }, []);

  const isCurrent = useCallback((id: ViewDefinition["id"]): boolean => viewRef.current === id, []);

  const { current: sessions, started } = useSessions(
    {
      refs,
      setters: state_,
      environment,
      environmentOf,
      foldPosture: currentFoldPosture,
      source: currentSource,
      isCurrent,
    },
    toolbarSettled,
  );

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
        });
        if (loaded.source !== "" && refs.source.current !== null) {
          // A link's source replaces the entry file's text: a share link is one document, and the entry is
          // the file it compiles under.
          const entry = workspace.state.files.find((file) => file.path === ENTRY_FILE) ?? null;
          if (entry !== null) {
            workspace.putFile({ ...entry, text: loaded.source });
            setRevision((value) => value + 1);
          }
        }
        state_.setControls({
          viewport: `${loaded.width || 320}x${loaded.height || 240}`,
          dpr: String(loaded.dpr ?? 1),
          theme: String(loaded.theme ?? "system"),
          foldPosture: String(loaded.foldPosture ?? ""),
        });
      } catch (error) {
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

  // ---- show the view: mount its module on first open, and tell every session which one is visible ----
  //
  // The view no longer decides the page's shape -- it selects which preview pane the right column shows --
  // so this effect is unchanged from #422 on purpose: the switch moved, its behaviour did not.
  useEffect(() => {
    for (const [id, candidate] of Object.entries(sessions)) {
      candidate?.setVisible?.(id === view);
    }
    const session = sessions[view];
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
    const session = sessions[viewRef.current];
    if (session && typeof session.run === "function") {
      void session.run();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const loadSampleForActiveView = useCallback(() => {
    const session = sessions[viewRef.current];
    const sample = session?.sampleSource;
    const entry = workspace.state.files.find((file) => file.path === ENTRY_FILE) ?? null;
    if (sample !== undefined && entry !== null) {
      // The view's sample is written into the *entry* buffer, because that is the file Run compiles when
      // the editor is showing a stylesheet. The page owns the buffers (#431); a session no longer reaches
      // into the editor.
      workspace.putFile({ ...entry, text: sample });
      setRevision((value) => value + 1);
    }
    session?.loadSample?.();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [workspace.state.files]);

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

  // ---- the workspace's own actions -----------------------------------------------------------------
  const onSelectTab = useCallback(
    (path: string) => {
      const file = workspace.state.files.find((candidate) => candidate.path === path) ?? null;
      if (file !== null) {
        workspace.openFile(file);
        setRevision((value) => value + 1);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [workspace.state.files],
  );

  const onEdit = useCallback(
    (text: string) => {
      workspace.writeBuffer(workspace.state.active, text);
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [workspace.state.active],
  );

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

  // Every mounted view hears about a toolbar move, not only the visible one: a Scene pane that is hidden
  // when the toolbar changes would otherwise show bounds from a viewport the toolbar no longer names the
  // moment it is opened. The first run is skipped -- it is the fragment decode, not a visitor's change.
  const firstControls = useRef(true);
  useEffect(() => {
    if (firstControls.current) {
      firstControls.current = false;
      return;
    }
    for (const session of Object.values(sessions)) {
      session?.onEnvironmentChange?.();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state.controls]);

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
      if (file !== null) {
        workspace.putFile(file);
        setRevision((value) => value + 1);
      }
      state_.setGalleryOpen(false);
      for (const session of Object.values(sessions)) {
        session?.onSample?.(sample.path);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [workspace.state.files],
  );

  const onShare = useCallback(async () => {
    state_.setShareStatus("encoding...");
    const compileTarget = targetRef.current;
    const shareState: ShareState = shareStateOf(
      compileTarget === null ? "" : textOfRef.current(compileTarget.path),
      state.controls,
      compileTarget === null ? DEFAULT_FILE : compileTarget.path,
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

      {/* The workspace: always three columns. The one thing that changes inside it is the preview column's
          own view switch, which is why the page's shape does not depend on `?view=`. */}
      <main className="workspace" id="workspace">
        <ProjectTree
          rows={workspace.rows}
          selected={workspace.state.selected}
          dirty={workspace.state.dirty}
          onSelect={workspace.selectRow}
          onToggleDir={workspace.toggleDir}
        />

        <CodeRegion
          open={workspace.state.open}
          files={workspace.state.files}
          active={workspace.state.active}
          dirty={workspace.state.dirty}
          text={activeText}
          revision={revision}
          sourceRef={refs.source}
          editorHintRef={refs.editorHint}
          onSelectTab={onSelectTab}
          onCloseTab={workspace.closeTab}
          onEdit={onEdit}
          onRun={runActiveView}
          onLoadSample={loadSampleForActiveView}
          runDisabled={!state.runEnabled}
          onKeyDown={onEditorKeyDown}
        />

        <PreviewColumn
          view={currentView}
          onSelectView={onSelectView}
          controls={state.controls}
          onControlChange={onControlChange}
          refs={refs}
        />
      </main>

      <StatusLine
        buildId={state.buildId}
        buildIdTitle={state.buildIdTitle}
        buildIdRef={refs.buildId}
        diagnosticsRef={refs.statusDiagnostics}
        frameRef={refs.statusFrame}
        file={target === null ? "" : target.path}
      />
    </>
  );
}
