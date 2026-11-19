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
import { projectFile, type ProjectFile } from "./lib/project";
import { entryOf, projectSnapshot } from "./lib/project-snapshot";
import {
  DEFAULT_VIEW,
  resolveView,
  viewOf,
  writeViewToUrl,
  type ProjectSnapshot,
  type ViewDefinition,
} from "./lib/views";
import { STARTUP_SAMPLE } from "./page-defaults";
import { createLiveEdit, type LiveEdit } from "./page-live";
import { setStatusText, usePageState, usePaneRefs } from "./page-state";
import { useSessions } from "./page-sessions";
import { useProjectIo } from "./page-project-io";
import { hasShareLink, sampleProjectFiles, useFirstView } from "./page-boot";
import { useViewOnlyLoader } from "./page-view-only";
import { useWorkspace } from "./page-workspace";
import { CodeRegion } from "./regions/CodeRegion";
import { GalleryRegion } from "./regions/GalleryRegion";
import { PreviewColumn } from "./regions/PreviewColumn";
import { ProjectBar } from "./regions/ProjectBar";
import { ProjectTree } from "./regions/ProjectTree";
import { PageHeader, type ControlValues } from "./regions/StudioChrome";
import { StatusLine } from "./regions/StatusLine";
import { useSkins, useThemeAttribute } from "./page-skins";

export function App() {
  const [view, setView] = useState<ViewDefinition["id"]>(() =>
    resolveView(globalThis.location?.search, DEFAULT_VIEW),
  );
  const [state, state_] = usePageState();
  const refs = usePaneRefs();
  const workspace = useWorkspace();
  const projectIo = useProjectIo(workspace);
  // The view the sessions' readiness callbacks are talking about (they are not re-created per view).
  const viewRef = useRef(view);
  viewRef.current = view;
  // The toolbar is settled once the fragment (or storage) has been decoded. The sessions wait for it, so a
  // `?view=scene#p5=<390x844>` link lays the scene out once, at 390x844.
  const [toolbarSettled, setToolbarSettled] = useState(false);
  // Bumped when the page itself replaces a buffer (a tab switch, a share link, a gallery pick), so the
  // editor's effect can tell a page write from a keystroke.
  const [revision, setRevision] = useState(0);

  useFirstView(projectIo.recentsLoaded, projectIo.recents.length, workspace.state.files.length, () =>
    state_.setGalleryOpen(true),
  );
  const onViewLoaded = useCallback(() => setRevision((value) => value + 1), []);
  useViewOnlyLoader(workspace, projectIo, onViewLoaded);

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

  // ---- the simulated device skin (issue #171's skins slice) ---------------------------------------
  //
  // The bundled manifests are loaded once and validated; the toolbar's `skin` value selects one, and
  // the selection's safe area is what the canvas session hands the engine. `useSkins` returns the
  // validated set, so a manifest that failed is never offered (section 5.2) and a link naming one
  // falls back to the bare canvas rather than to a frame nobody validated.
  const skins = useSkins(state.controls.skin ?? "");
  // The Theme control is real since this slice: it writes `<html data-theme>`, which is what retints
  // the frame and the canvas in one cascade (section 5.2's retint rule).
  useThemeAttribute(state.controls.theme);
  const currentSafeArea = useCallback(() => skins.safeArea, [skins.safeArea]);

  // The entry's component the preview shows (issue #391). `""` is the entry's first; the canvas reports the
  // compiler's list after each apply, and the dropdown offers it. Read through a ref so picking one never
  // re-creates a session.
  const [previewComponent, setPreviewComponent] = useState("");
  const [componentList, setComponentList] = useState<{ names: string[]; chosen: string }>({
    names: [],
    chosen: "",
  });
  const previewComponentRef = useRef(previewComponent);
  previewComponentRef.current = previewComponent;
  const currentComponent = useCallback(() => previewComponentRef.current, []);
  const onComponents = useCallback((names: string[], chosen: string) => {
    setComponentList((previous) =>
      previous.chosen === chosen &&
      previous.names.length === names.length &&
      previous.names.every((name, index) => name === names[index])
        ? previous
        : { names, chosen },
    );
  }, []);

  // ---- what Run previews: the whole project, read at Run time ---------------------------------------
  //
  // The sessions ask one reader and get the project's sources, its stylesheets and the entry
  // `tessera.json` names; they stage all of it into the module. Which file the editor shows does not
  // decide what is previewed, and there is no fallback entry: a project that names none previews nothing
  // and the modules say why. The reader goes through refs so a keystroke never re-creates a session, and
  // it is read when Run (or the live-edit debounce) fires, so the buffers are always the current ones.
  const filesRef = useRef(workspace.state.files);
  filesRef.current = workspace.state.files;
  // The file the editor is showing, for the live edit's "did the keystroke land in a source file?" test
  // and for a diagnostic's "show in source" affordance.
  const activeRef = useRef(workspace.state.active);
  activeRef.current = workspace.state.active;
  const textOfRef = useRef(workspace.textOf);
  textOfRef.current = workspace.textOf;
  const entryPath = workspace.entry.path;
  const currentSource = useCallback(
    (): ProjectSnapshot => projectSnapshot(filesRef.current, textOfRef.current, activeRef.current),
    [],
  );

  const isCurrent = useCallback((id: ViewDefinition["id"]): boolean => viewRef.current === id, []);

  const { current: sessions, started } = useSessions(
    {
      refs,
      setters: state_,
      environment,
      environmentOf,
      foldPosture: currentFoldPosture,
      safeArea: currentSafeArea,
      component: currentComponent,
      onComponents,
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
          skin: "",
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
        });
      } catch (error) {
        // The seeded project is what the page shows when the link is broken (#431).
        state_.setControls({
          viewport: "320x240",
          dpr: "1",
          theme: "system",
          foldPosture: "",
          skin: "",
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

  // ---- the live edit (issue #444) -------------------------------------------------------------------
  //
  // The developer does not press Run. One controller owns the 150 ms debounce, and every path that
  // changes the source goes through it (`page-live.ts` explains why there is exactly one timer):
  //
  //   * a keystroke      -> `schedule()`, applied once 150 ms after the burst ends;
  //   * `Ctrl+Enter`     -> `applyNow()`, immediately, cancelling the debounce;
  //   * **Restart app**  -> `restart()`, a cold start of every mounted pane.
  //
  // The page hands it readers rather than values, for the same reason the sessions do: this object is
  // created once and must never be recreated by a render (a second controller would be a second timer).
  const live = useRef<LiveEdit | null>(null);
  if (live.current === null) {
    live.current = createLiveEdit({
      sessions: () => sessions,
      currentView: () => viewRef.current,
      // The file the *keystroke* landed in, which is the file the spec's reset case is about: typing in
      // `theme.css` edits a non-source project file, so it restarts rather than live-applying. Only the
      // path is read, never the text.
      editedFile: () => activeRef.current,
      // A stylesheet is part of the previewed project, so an edit to one is a live apply like a source
      // edit; the config (it names the entry) and the other kinds restart instead.
      kindOf: (path: string) => {
        const file = workspace.state.files.find((candidate) => candidate.path === path);
        return file?.kind === "stylesheet" ? "source" : (file?.kind ?? "other");
      },
      onStateChange: (next) => state_.setApplying(next.applying),
    });
  }

  // ---- show the view: mount its module on first open, and tell every session which one is visible ----
  //
  // The view no longer decides the page's shape -- it selects which preview pane the right column shows --
  // so this effect is unchanged from #422 on purpose: the switch moved, its behaviour did not.
  useEffect(() => {
    for (const [id, candidate] of Object.entries(sessions)) {
      candidate?.setVisible?.(id === view);
    }
    const session = sessions[view];
    // A view that is already mounted can be restarted; one whose module is still loading cannot, and its
    // own `onReady` enables the button when it is.
    state_.setRunEnabled(Boolean(session) && started.current[view] === true);
    if (!session || started.current[view]) {
      return;
    }
    started.current[view] = true;
    void session
      .load()
      // **The first automatic apply (issue #444).** The page used to open on "not started; press Run",
      // with an empty preview as its resting state; the spec says the first load starts the preview
      // automatically once the module is ready. `applyNow()` is exactly the debounce's action with the
      // wait removed, so the first paint is not a different code path from every later edit -- and it is
      // what acquires the WebGPU device (lazily, here, never on page load). A source the compiler rejects
      // leaves the panes' own named states alone, which is why this is not wrapped in a second handler.
      .then(() => live.current?.applyNow())
      .catch((error: unknown) => {
        // A view module that fails to import is that view's problem: it is reported in that pane's own
        // status line, and the other views keep working. The console carries the detail because a pane
        // whose module never loaded may have no status node to write into.
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
  //
  // **Restart app** is the cold start (spec section 3): it discards state and re-runs from a launch. It
  // is deliberately *not* what an edit does -- `apply` is -- so a developer who wants the counter back
  // at 0 has one button for it, and the button is named for what it does.
  const restartActiveView = useCallback(() => {
    void live.current?.restart();
  }, []);

  const loadSampleForActiveView = useCallback(() => {
    const session = sessions[viewRef.current];
    const sample = session?.sampleSource;
    const entry = workspace.state.files.find((file) => file.path === entryPath) ?? null;
    if (sample !== undefined && entry !== null) {
      // The view's sample is written into the *entry* buffer, because the entry is what Run previews.
      // The page owns the buffers (#431); a session no longer reaches into the editor.
      workspace.putFile({ ...entry, text: sample });
      setRevision((value) => value + 1);
    }
    session?.loadSample?.();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [workspace.state.files]);

  // A load replaces the whole project, so the preview starts over from the new entry and the editor
  // takes the new active file's text. Skipped at epoch 0, which is the seed, not a load.
  useEffect(() => {
    if (projectIo.epoch === 0) {
      return;
    }
    setRevision((value) => value + 1);
    setPreviewComponent("");
    previewComponentRef.current = "";
    restartActiveView();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [projectIo.epoch]);

  // Picking another component re-evaluates the entry with that name; the canvas draws one frame.
  const onPickComponent = useCallback(
    (name: string) => {
      setPreviewComponent(name);
      previewComponentRef.current = name;
      sessions.canvas?.onSample?.(entryPath ?? "");
    },
    [sessions],
  );

  // Save is explicit (no autosave); the editor's keymap already stopped the browser's own "save page".
  const onEditorSave = useCallback(() => projectIo.save(), [projectIo.save]);
  // "Apply now", not "run": Ctrl+Enter skips the debounce, it does not reset the app.
  const onEditorApplyNow = useCallback(() => {
    void live.current?.applyNow();
  }, []);

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
      // A locked file opens read-only, so this is not reached for one; the guard keeps it so.
      if (workspace.state.files.find((file) => file.path === workspace.state.active)?.editable !== true) {
        return;
      }
      workspace.writeBuffer(workspace.state.active, text);
      // The live edit (issue #444): the trigger owns the debounce, so this is one call per keystroke and
      // one apply per burst. It reads the file the workspace compiles through the refs above, so a
      // keystroke in a stylesheet tab applies nothing until the spec's restart case fires.
      live.current?.schedule();
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
      if (file !== null && workspace.state.files.length === 0) {
        // Nothing is open (the studio no longer boots on a demo project): the sample becomes a project.
        projectIo.openSample(sample.component, sampleProjectFiles(file.path, sample.text, sample.component));
      } else if (file !== null) {
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
    const shareState: ShareState = shareStateOf(
      entryPath === null ? "" : textOfRef.current(entryPath),
      state.controls,
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
      <main
        className="workspace"
        id="workspace"
        onDragOver={(event) => {
          if (event.dataTransfer.types.includes("Files")) {
            event.preventDefault();
          }
        }}
        onDrop={(event) => {
          const dropped = event.dataTransfer.files[0];
          if (dropped !== undefined && /\.zip$/i.test(dropped.name)) {
            event.preventDefault();
            projectIo.openZip(dropped);
          }
        }}
      >
        <ProjectTree
          rows={workspace.rows}
          selected={workspace.state.selected}
          dirty={workspace.state.dirty}
          stale={projectIo.state.stale}
          onSelect={workspace.selectRow}
          onToggleDir={workspace.toggleDir}
          toolbar={<ProjectBar io={projectIo} dirtyCount={new Set([...workspace.state.dirty, ...workspace.state.removed]).size} />}
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
          onRestart={restartActiveView}
          onLoadSample={loadSampleForActiveView}
          restartDisabled={!state.runEnabled}
          applying={state.applying}
          onSave={onEditorSave}
          onApplyNow={onEditorApplyNow}
          stale={projectIo.state.stale}
        />

        <PreviewColumn
          view={currentView}
          onSelectView={onSelectView}
          controls={state.controls}
          onControlChange={onControlChange}
          refs={refs}
          components={componentList.names}
          component={componentList.chosen}
          onPickComponent={onPickComponent}
          skins={skins.skins}
          skin={skins.selected}
        />
      </main>

      <StatusLine
        buildId={state.buildId}
        buildIdTitle={state.buildIdTitle}
        buildIdRef={refs.buildId}
        diagnosticsRef={refs.statusDiagnostics}
        frameRef={refs.statusFrame}
        file={entryPath ?? ""}
      />
    </>
  );
}
