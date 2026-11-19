// SPDX-License-Identifier: MIT
// W0 spike (throwaway, deleted when W5 lands): flexlayout-react against the layout spec's hard constraints
// (4.1 canvas never re-parented, 4.2 lazy + hidden never draws, 4.3 failure isolation, 4.4 ARIA/keyboard).
// Driven by scripts/ci/workbench_dock_spike.mjs; exposes window.__spike for the measurements.
import { Component, useEffect, useRef, useState, type ReactNode } from "react";
import { createRoot } from "react-dom/client";
import { Actions, DockLocation, Layout, Model, type TabNode, type IJsonModel } from "flexlayout-react";
import "flexlayout-react/style/light.css";
import type { CanvasSession, SpikeStats } from "./canvas-module";

const stats: SpikeStats = { frames: 0, importedAt: null, contextCreations: 0, gpu: false };
const probe = { canvasMounts: 0, canvasRemovals: 0, canvasNodes: [] as HTMLCanvasElement[], visibleEvents: [] as string[] };

const bottomMode = new URLSearchParams(location.search).get("bottom") === "border" ? "border" : "tabset";

const diag = { type: "tab", id: "diagnostics", name: "Diagnostics", component: "diagnostics", enableClose: false } as const;
const boom = { type: "tab", id: "boom", name: "Faulty", component: "boom" } as const;

const json: IJsonModel = {
  global: { tabEnableRenderOnDemand: false, tabSetEnableMaximize: true, tabEnableClose: true, tabEnablePopout: false },
  borders:
    bottomMode === "border"
      ? [{ type: "border", location: "bottom", size: 180, selected: -1, children: [diag, boom] }]
      : [],
  layout: {
    type: "row",
    children: [
      {
        type: "row",
        weight: 100,
        children: [
          {
            type: "row",
            weight: 72,
            children: [
              { type: "tabset", id: "g-project", weight: 18, children: [{ type: "tab", id: "project", name: "Project", component: "project" }] },
              { type: "tabset", id: "g-editor", weight: 41, children: [{ type: "tab", id: "editor", name: "Editor", component: "editor", enableClose: false }] },
              {
                type: "tabset",
                id: "g-preview",
                weight: 41,
                selected: new URLSearchParams(location.search).get("bg") === "1" ? 1 : 0,
                children: [
                  { type: "tab", id: "canvas", name: "Canvas", component: "canvas" },
                  { type: "tab", id: "scene", name: "Scene", component: "scene" },
                ],
              },
            ],
          },
          ...(bottomMode === "tabset"
            ? [{ type: "tabset" as const, id: "g-bottom", weight: 28, children: [diag, boom] }]
            : []),
        ],
      },
    ],
  },
};

class Boundary extends Component<{ id: string; children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }
  render(): ReactNode {
    if (!this.state.failed) return this.props.children;
    return (
      <div role="alert" data-panel-failed={this.props.id} className="panel-body">
        This panel failed
        <button type="button" onClick={() => this.setState({ failed: false })}>Reload panel</button>
      </div>
    );
  }
}

function CanvasPanel({ node }: { node: TabNode }): ReactNode {
  const host = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = host.current;
    if (!el) return;
    const canvas = document.createElement("canvas");
    canvas.id = "spike-canvas";
    canvas.style.cssText = "width:100%;height:100%;display:block";
    el.appendChild(canvas);
    probe.canvasMounts += 1;
    probe.canvasNodes.push(canvas);
    let session: CanvasSession | null = null;
    let visible = node.isVisible();
    let importing: Promise<void> | null = null;
    const apply = (): void => {
      if (visible && !importing) {
        importing = import("./canvas-module").then(async (m) => {
          stats.importedAt = performance.now();
          session = await m.startCanvas(canvas, stats);
          session.visible(visible);
        });
      } else {
        session?.visible(visible);
      }
    };
    node.setEventListener("visibility", (p: { visible: boolean }) => {
      visible = p.visible;
      probe.visibleEvents.push(`${node.getId()}:${p.visible}`);
      apply();
    });
    apply();
    return () => {
      node.removeEventListener("visibility");
      session?.dispose();
      if (canvas.parentNode === el) el.removeChild(canvas);
    };
  }, [node]);
  return <div ref={host} data-panel-body="canvas" style={{ width: "100%", height: "100%" }} />;
}

function Boom(): ReactNode {
  const [bad, setBad] = useState(false);
  if (bad) throw new Error("panel render failure (spike)");
  return (
    <div className="panel-body">
      <button type="button" id="boom-button" onClick={() => setBad(true)}>Throw in this panel</button>
    </div>
  );
}

function factory(node: TabNode): ReactNode {
  const c = node.getComponent();
  const body = (inner: ReactNode): ReactNode => (
    <Boundary id={node.getId()}>{inner}</Boundary>
  );
  switch (c) {
    case "project":
      return body(<ul className="panel-body" data-panel-body="project"><li>App.tsx</li><li>Home.tsx</li></ul>);
    case "editor":
      return body(<div className="panel-body"><textarea id="editor-input" aria-label="Editor" style={{ width: "90%", height: "80%" }} /></div>);
    case "canvas":
      return body(<CanvasPanel node={node} />);
    case "scene":
      return body(<div className="panel-body" data-panel-body="scene">Scene tree</div>);
    case "diagnostics":
      return body(<div className="panel-body" data-panel-body="diagnostics">0 problems</div>);
    case "boom":
      return body(<Boom />);
    default:
      return null;
  }
}

function App(): ReactNode {
  const [model] = useState(() => Model.fromJson(json));
  const [changes, setChanges] = useState(0);
  useEffect(() => {
    const w = window as unknown as Record<string, unknown>;
    w.__spike = { stats, probe, model, Actions, DockLocation, bottomMode };
  }, [model]);
  return (
    <div style={{ position: "absolute", inset: 0 }} data-changes={changes}>
      <Layout
        model={model}
        factory={factory}
        supportsPopout={false}
        onModelChange={() => setChanges((n) => n + 1)}
        keyMap={{ closeTab: "Alt+W" }}
      />
    </div>
  );
}

// Keyboard docking: an adapter-owned command (spec 4.4 "keyboard panel moves are commands"), shown here as
// Alt+Shift+Arrow on a focused tab. flexlayout has no keyboard move of its own, so this is the pattern W5 uses.
const live = document.createElement("div");
live.setAttribute("aria-live", "polite");
live.id = "spike-live";
document.body.appendChild(live);
window.addEventListener("keydown", (event) => {
  if (!event.altKey || !event.shiftKey) return;
  const button = (event.target as Element | null)?.closest?.("[role=tab]");
  const panel = button?.id.replace("flexlayout-tabbutton-", "");
  if (!panel) return;
  const w = window as unknown as { __spike?: { model: Model } };
  const model = w.__spike?.model;
  if (!model) return;
  const where: Record<string, [string, DockLocation, string]> = {
    ArrowDown: ["g-bottom", DockLocation.CENTER, "the bottom"],
    ArrowRight: ["g-preview", DockLocation.RIGHT, "the right"],
    ArrowLeft: ["g-editor", DockLocation.LEFT, "the left of the editor"],
  };
  const hit = where[event.key];
  if (!hit) return;
  event.preventDefault();
  model.doAction(Actions.moveNode(panel, hit[0], hit[1], -1));
  live.textContent = `${panel} moved to ${hit[2]}`;
  requestAnimationFrame(() => document.getElementById(`flexlayout-tabbutton-${panel}`)?.focus());
});

new MutationObserver((records) => {
  for (const r of records) {
    for (const n of Array.from(r.removedNodes)) {
      if (n instanceof HTMLCanvasElement || (n instanceof Element && n.querySelector("canvas"))) probe.canvasRemovals += 1;
    }
  }
}).observe(document.documentElement, { childList: true, subtree: true });

createRoot(document.getElementById("root") as HTMLElement).render(
  <App />,
);
