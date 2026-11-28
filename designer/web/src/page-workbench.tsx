// SPDX-License-Identifier: MIT

// The workbench surface of the page (issue #516, W5 #560): every region the studio had, as a dock panel.
//
// Before the dock, `App.tsx` rendered three fixed columns and a bottom strip. The same components now fill the
// panels of one dock the viewer arranges (`dock/`): the project tree, the editor, Canvas, Scene, Diagnostics, Problems
// and the three bottom panels. This file only maps panel ids to the components and the props the page already
// had for them; it owns no state, so nothing about *when* a panel's session starts or what a keystroke does
// changed (`page-panels.ts` and `page-sessions.ts` carry that).

import type { ReactNode } from "react";
import { WorkbenchDock } from "./dock/WorkbenchDock";
import type { Workbench } from "./dock/use-workbench";
import type { MockConsole } from "./lib/mock-console";
import type { DeviceProfile } from "./lib/device-profile";
import type { ProjectFile } from "./lib/project";
import type { ProjectIo } from "./page-project-io";
import type { PlatformControls } from "./page-platform";
import type { ResourceControls } from "./page-resources";
import type { ProblemsHandle } from "./page-problems";
import type { PageRefs } from "./page-state";
import type { Workspace } from "./page-workspace";
import type { Skin } from "./views/skins";
import { CallTracePanel } from "./regions/CallTracePanel";
import { CodeRegion } from "./regions/CodeRegion";
import { MockConsolePanel } from "./regions/MockConsolePanel";
import { PlatformPanel } from "./regions/PlatformDock";
import { ResourcePanel } from "./regions/ResourcePanel";
import { CanvasPanel, DiagnosticsPanel, ScenePanel } from "./regions/PreviewPanels";
import { ProblemsPanel } from "./regions/ProblemsPanel";
import { ProjectBar } from "./regions/ProjectBar";
import { ProjectTree } from "./regions/ProjectTree";

export interface WorkbenchSurfaceProps {
  workbench: Workbench;
  workspace: Workspace;
  projectIo: ProjectIo;
  refs: PageRefs;
  /** The Problems store and its click-to-jump. */
  problems: ProblemsHandle;
  activeText: string;
  revision: number;
  runEnabled: boolean;
  applying: boolean;
  onSelectTab: (path: string) => void;
  onEdit: (text: string) => void;
  onRestart: () => void;
  onSave: () => void;
  onApplyNow: () => void;
  skin: Skin | null;
  /** The two nodes the platform panel supplies: the overlay over the canvas and the hardware strip under it. */
  overlay: ReactNode;
  strip: ReactNode;
  platform: PlatformControls;
  /** The fake resources of the open component (issue #657), shown under the platform events. */
  resources: ResourceControls;
  profile: DeviceProfile | null;
  mocks: MockConsole;
  files: readonly ProjectFile[];
  onReloadPanel: (panelId: string) => void;
  /** The welcome view, passed while no project is open; the editor column shows it. */
  welcome?: ReactNode;
}

/** A bottom panel's scroll frame (the old dock panel's `overflow`, now per panel). */
function Scroll({ children }: { children: ReactNode }) {
  return <div className="panel-scroll">{children}</div>;
}

export function WorkbenchSurface(props: WorkbenchSurfaceProps): ReactNode {
  const { workspace, projectIo, refs, mocks } = props;
  const mockFiles = props.files.filter((file) => file.kind === "mock");
  const panels: Record<string, ReactNode> = {
    project: (
      <ProjectTree
        rows={workspace.rows}
        selected={workspace.state.selected}
        dirty={workspace.state.dirty}
        stale={projectIo.state.stale}
        onSelect={workspace.selectRow}
        onToggleDir={workspace.toggleDir}
        toolbar={<ProjectBar io={projectIo} />}
      />
    ),
    editor: (
      <CodeRegion
        open={workspace.state.open}
        files={workspace.state.files}
        active={workspace.state.active}
        dirty={workspace.state.dirty}
        text={props.activeText}
        revision={props.revision}
        sourceRef={refs.source}
        onSelectTab={props.onSelectTab}
        onCloseTab={workspace.closeTab}
        onEdit={props.onEdit}
        applying={props.applying}
        onSave={props.onSave}
        onApplyNow={props.onApplyNow}
        stale={projectIo.state.stale}
        empty={props.welcome ?? null}
        caretRef={refs.statusCaret}
      />
    ),
    canvas: (
      <CanvasPanel
        refs={refs}
        skin={props.skin}
        overlay={props.overlay}
        strip={props.strip}
      />
    ),
    scene: <ScenePanel refs={refs} />,
    diagnostics: <DiagnosticsPanel refs={refs} />,
    problems: <ProblemsPanel store={props.problems.store} visible={props.workbench.visible.has("problems")} onJump={props.problems.jump} />,
    "platform-events": (
      <Scroll>
        <PlatformPanel profile={props.profile} platform={props.platform} onRestart={props.onRestart} />
        <ResourcePanel resources={props.resources} />
      </Scroll>
    ),
    "mock-console": (
      <Scroll>
        <MockConsolePanel mocks={mocks} hasMockFiles={mockFiles.length > 0} />
      </Scroll>
    ),
    "call-trace": (
      <Scroll>
        <CallTracePanel mocks={mocks} />
      </Scroll>
    ),
  };
  return <WorkbenchDock workbench={props.workbench} panels={panels} onReloadPanel={props.onReloadPanel} />;
}
