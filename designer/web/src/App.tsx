// SPDX-License-Identifier: MIT

// The studio page: the workbench chrome (menu bar, toolbar, palette, About), the dock of panels (project tree,
// editor, Canvas, Scene, Diagnostics, bottom panels) and the status bar, wired to the page's state.
//
// **What React renders, and what it does not.** React renders the chrome, the tree, the tabs and the status
// items; the engine still draws exactly one thing, the preview canvas, and every subtree a Wasm module produces
// is a childless leaf a *session* fills (`src/views/*-session.ts`, ADR-054 decision 3). A preview crash loses a
// pane, not the editor.
//
// **Why the sessions live in a ref and not in state.** A session owns a Wasm module instance, a WebGPU device
// and an animation-frame callback. None of that may be created by a render or destroyed by a re-render, so each
// is created once against the committed DOM nodes (`page-sessions.ts`). The page's React state is exactly what a
// visitor changes by hand: the workspace (`page-workspace.ts`), the toolbar and the build id.
// Startup, the toolbar handlers and the sample action are hooks of their own (`page-startup.ts`,
// `page-toolbar.ts`, `page-sample.ts`) to keep this file under the size limit.
//
// **Laziness** survives verbatim: the Diagnostics module is the only one on the first-paint path; Scene's is
// imported the first time that pane is shown; Canvas' is imported on its first open and its WebGPU device is
// acquired on the first Run there.


import { useCallback, useEffect, useRef, useState } from "react";
import { projectSnapshot } from "./lib/project-snapshot";
import {
  DEFAULT_VIEW,
  resolveView,
  type ProjectSnapshot,
  type ViewDefinition,
} from "./lib/views";
import { customProfile } from "./lib/device-profile";
import { unavailableReply, type PlatformEventRequest, type PlatformReply } from "./lib/platform-events";
import { createLiveEdit, type LiveEdit } from "./page-live";
import { usePageState, usePaneRefs } from "./page-state";
import { useStartup } from "./page-startup";
import { useToolbarControls } from "./page-toolbar";
import { useSample } from "./page-sample";
import { useSessions } from "./page-sessions";
import { useProjectIo } from "./page-project-io";
import { useViewOnlyLoader } from "./page-view-only";
import { useWorkspace } from "./page-workspace";
import { WelcomeView } from "./regions/WelcomeView";
import { useWorkbench } from "./dock/use-workbench";
import { useMockConsole } from "./lib/mock-console";
import { usePanelVisibility, useViewFromUrl } from "./page-panels";
import { useProblems, useProblemsStore } from "./page-problems";
import { WorkbenchSurface } from "./page-workbench";
import { WorkbenchChrome } from "./regions/WorkbenchChrome";
import { useWorkbench as useCommands } from "./page-commands";
import { StatusLine } from "./regions/StatusLine";
import { useSkins, useThemeAttribute } from "./page-skins";
import { usePlatform } from "./page-platform";
import { useProfiles } from "./page-profiles";
import { AppStateOverlay, HardwareStrip } from "./regions/DeviceControls";

export function App() {
  const [view, setView] = useState<ViewDefinition["id"]>(() =>
    resolveView(globalThis.location?.search, DEFAULT_VIEW),
  );
  const [state, state_] = usePageState();
  const refs = usePaneRefs();
  const workspace = useWorkspace();
  const projectIo = useProjectIo(workspace);
  // The dock (W5, #560): the layout model, its persistence and which panels are open and visible.
  const workbench = useWorkbench();
  const problemsStore = useProblemsStore();
  useViewFromUrl(workbench);
  // The view panels that are open, so a session exists exactly while its panel does; `reloadEpoch` is what
  // "Reload panel" bumps to bind a session to the re-rendered content.
  const [reloadEpoch, setReloadEpoch] = useState(0);
  const present = ["diagnostics", "scene", "canvas"].filter((id) => workbench.open.has(id)).join(",") + `#${reloadEpoch}`;
  // The view the sessions' readiness callbacks are talking about (they are not re-created per view).
  const viewRef = useRef(view);
  viewRef.current = view;
  // The toolbar is settled once the stored toolbar has been read. The sessions wait for it, so a
  // stored 390x844 lays the scene out once, at 390x844.
  const [toolbarSettled, setToolbarSettled] = useState(false);
  // Bumped when the page itself replaces a buffer (a tab switch, a gallery pick), so the
  // editor's effect can tell a page write from a keystroke.
  const [revision, setRevision] = useState(0);
  // Bumped when a `*.mock.ts` buffer is edited, so the mock sandbox worker is recreated (ADR-055).
  const [mockRevision, setMockRevision] = useState(0);

  const onViewLoaded = useCallback(() => setRevision((value) => value + 1), []);
  useViewOnlyLoader(workspace, projectIo, onViewLoaded);

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

  const sessionHandle = useSessions(
    {
      refs,
      setters: state_,
      environment,
      environmentOf,
      foldPosture: currentFoldPosture,
      safeArea: currentSafeArea,
      component: currentComponent,
      onComponents,
      // With no project open the compiler's answer is "the project names no entry file": not a problem to list.
      onDiagnostics: (diagnostics) =>
        filesRef.current.length === 0 ? problemsStore.reset() : problemsStore.publish(diagnostics),
      source: currentSource,
      isCurrent,
    },
    toolbarSettled,
    present,
  );
  const sessions = sessionHandle.current;

  useStartup({ state_, setToolbarSettled });


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

  // ---- the device profile and the platform panel (issues #447 and #448) ----------------------------
  //
  // The profile decides the toolbar's size, DPR, skin and fold and whether the app restarts; the platform
  // hook sends normalized events through the canvas session and keeps the log. Both read the page through
  // refs where a callback outlives a render, for the reason the sessions do.
  const profiles = useProfiles(state.controls);
  const [keepState, setKeepState] = useState(false);
  const controlsRef = useRef(state.controls);
  controlsRef.current = state.controls;
  const profilesRef = useRef(profiles);
  profilesRef.current = profiles;
  const skinsRef = useRef(skins.skins);
  skinsRef.current = skins.skins;
  const keepStateRef = useRef(keepState);
  keepStateRef.current = keepState;

  const platform = usePlatform({
    deliver: (request: PlatformEventRequest): PlatformReply =>
      sessions.canvas?.platformEvent?.(request) ??
      unavailableReply(request.type, "the canvas preview is not available"),
    coldStart: () => live.current?.restart(),
    // An orientation or fold event moved a layout input the engine already applied in place; the toolbar
    // holds the same facts, so it follows rather than undoing the engine on its next environment change.
    onLayout: (request) => {
      if (request.type === "orientation") {
        state_.setControls((previous) => {
          const [width, height] = previous.viewport.split("x");
          return { ...previous, viewport: `${height}x${width}` };
        });
      } else if (request.type === "fold") {
        state_.setControls((previous) => ({
          ...previous,
          foldPosture: request.value === "none" ? "" : request.value,
        }));
      }
    },
  });
  const platformReset = platform.reset;
  const deviceProfile =
    profiles.selected ??
    customProfile(
      { width: environmentOf().width, height: environmentOf().height },
      environmentOf().dpr,
      null,
    );

  // ---- panel visibility: mount a module on first show, and tell every session whether it is visible ----
  //
  // The dock's visible set decides this now (several panels can be visible at once); see `page-panels.ts`.
  usePanelVisibility({
    visible: workbench.visible,
    sessions: sessionHandle,
    live,
    refs,
    toolbarSettled,
    present,
    viewRef,
    setView,
    setRunEnabled: state_.setRunEnabled,
  });

  // ---- the shared buttons and the editor's keyboard shortcut ----------------------------------------
  //
  // **Restart app** is the cold start (spec section 3): it discards state and re-runs from a launch. It
  // is deliberately *not* what an edit does -- `apply` is -- so a developer who wants the counter back
  // at 0 has one button for it, and the button is named for what it does.
  const restartActiveView = useCallback(() => {
    // A cold start ends the platform log: it is "what happened since the app launched", the only span a
    // replay can reproduce from a launch (`page-platform.ts`).
    platformReset();
    void live.current?.restart();
  }, [platformReset]);

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

  const problems = useProblems({ store: problemsStore, workbench, workspace, refs, onSelectTab });

  const onEdit = useCallback(
    (text: string) => {
      // A locked file opens read-only, so this is not reached for one; the guard keeps it so.
      if (workspace.state.files.find((file) => file.path === workspace.state.active)?.editable !== true) {
        return;
      }
      workspace.writeBuffer(workspace.state.active, text);
      if (/\.mock\.ts$/i.test(workspace.state.active)) {
        setMockRevision((value) => value + 1);
      }
      // The live edit (issue #444): the trigger owns the debounce, so this is one call per keystroke and
      // one apply per burst. It reads the file the workspace compiles through the refs above, so a
      // keystroke in a stylesheet tab applies nothing until the spec's restart case fires.
      live.current?.schedule();
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [workspace.state.active],
  );

  const { onCustomSize, onControlChange } = useToolbarControls({
    state_,
    controls: state.controls,
    controlsRef,
    profilesRef,
    skinsRef,
    keepStateRef,
    sessions,
    restart: restartActiveView,
  });


  const { onSample } = useSample({ workspace, projectIo, setRevision, sessions });

  // The sandbox worker starts only while a panel that shows its output is visible *and* the project has a mock
  // file (spec section 2.4), so a project without mocks, or a collapsed panel area, pays nothing.
  const mocks = useMockConsole(
    workbench.visible.has("mock-console") || workbench.visible.has("call-trace"),
    workspace.state.files,
    workspace.textOf,
    mockRevision,
  );

  // "Reload panel" on a view panel: drop its session and bind a new one to the re-rendered nodes.
  const onReloadPanel = useCallback(
    (panelId: string) => {
      const id = panelId as ViewDefinition["id"];
      if (sessions[id] !== undefined) {
        sessions[id]?.dispose?.();
        delete sessions[id];
        sessionHandle.started.current[id] = false;
      }
      setReloadEpoch((value) => value + 1);
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  const commandHost = useCommands({
    dock: workbench,
    io: projectIo,
    dirtyCount: new Set([...workspace.state.dirty, ...workspace.state.removed]).size,
    hasProject: workspace.state.files.length > 0,
    controls: state.controls,
    keepState,
    setKeepState,
    skins: skins.skins,
    profiles: profiles.profiles,
    profileId: state.controls.profile,
    components: componentList,
    runEnabled: state.runEnabled,
    pickSample: onSample,
    restart: restartActiveView,
    applyNow: onEditorApplyNow,
    setControl: onControlChange,
    setCustomSize: onCustomSize,
    pickComponent: onPickComponent,
    files: workspace.state.files,
    openFile: onSelectTab,
    buildId: state.buildId,
    refs,
  });

  return (
    <>
      <WorkbenchChrome workbench={commandHost} zen={workbench.layout.shell.zen} />

      {/* The workspace is the dock (W5): panels the viewer arranges, not three fixed columns. */}
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
        <WorkbenchSurface
          workbench={workbench}
          workspace={workspace}
          projectIo={projectIo}
          refs={refs}
          activeText={activeText}
          revision={revision}
          runEnabled={state.runEnabled}
          applying={state.applying}
          problems={problems}
          onSelectTab={onSelectTab}
          onEdit={onEdit}
          onRestart={restartActiveView}
          onSave={onEditorSave}
          onApplyNow={onEditorApplyNow}
          skin={skins.selected}
          overlay={
            <AppStateOverlay
              state={platform.state}
              disabled={platform.replaying}
              onAction={(type) => void platform.send(type)}
            />
          }
          strip={
            <HardwareStrip
              profile={profiles.selected}
              disabled={platform.replaying}
              onPress={(type) => void platform.send(type)}
            />
          }
          platform={platform}
          profile={deviceProfile}
          mocks={mocks}
          files={workspace.state.files}
          onReloadPanel={onReloadPanel}
          welcome={
            workspace.state.files.length === 0 ? (
              <WelcomeView onPick={onSample} run={(id) => void commandHost.run(id)} />
            ) : null
          }
        />
      </main>

      <StatusLine
        workbench={commandHost}
        problems={problems.store}
        caretRef={refs.statusCaret}
        activePath={workspace.state.active}
        entryPath={entryPath ?? ""}
        problemsOpen={workbench.visible.has("problems")}
        hidden={!workbench.layout.shell.statusBar || workbench.layout.shell.zen}
      />
    </>
  );
}
