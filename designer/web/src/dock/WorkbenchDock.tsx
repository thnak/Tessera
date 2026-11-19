// SPDX-License-Identifier: MIT

// The dock the page renders: the flexlayout adapter plus every open panel's content, portaled into its
// persistent host (`hosts.ts`). Closed panels have no content mounted, so closing Canvas releases what it
// held; collapsed and background panels stay mounted in their hosts, which is what lets Diagnostics fill the
// status bar's count while the panel area is hidden.
//
// Without a document (the Node chrome gate renders the page with `react-dom/server`) there is no dock to
// measure, so the open panels render inline in reading order: same ids, no library.

import { createPortal } from "react-dom";
import type { ReactNode } from "react";
import { openPanels, PANELS, type PanelDef } from "../workbench";
import { FlexDock } from "./FlexDock";
import { panelHost } from "./hosts";
import { PanelBoundary } from "./PanelBoundary";
import type { Workbench } from "./use-workbench";

export interface WorkbenchDockProps {
  workbench: Workbench;
  /** Content per panel id; a panel with no entry is open but empty. */
  panels: Readonly<Record<string, ReactNode>>;
  defs?: readonly PanelDef[];
  onReloadPanel?: (panelId: string) => void;
}

export function WorkbenchDock({ workbench, panels, defs = PANELS, onReloadPanel }: WorkbenchDockProps): ReactNode {
  const titleOf = (id: string): string => defs.find((def) => def.id === id)?.title ?? id;
  const wrap = (id: string, content: ReactNode): ReactNode => (
    <PanelBoundary key={id} panelId={id} title={titleOf(id)} onReload={() => onReloadPanel?.(id)}>
      {content}
    </PanelBoundary>
  );

  if (typeof document === "undefined") {
    return (
      <div className="dock-ssr">
        {openPanels(workbench.layout).map((id) => (
          <section key={id} data-panel={id} className="panel-host">
            {wrap(id, panels[id] ?? null)}
          </section>
        ))}
      </div>
    );
  }

  return (
    <>
      <FlexDock
        layout={workbench.rendered}
        defs={defs}
        phone={workbench.phone}
        activeGroup={workbench.activeGroup}
        onOperation={workbench.dispatch}
        announce={workbench.announce}
      />
      {[...workbench.open].map((id) => {
        const host = panelHost(id);
        return host === null ? null : createPortal(wrap(id, panels[id] ?? null), host, id);
      })}
      <div className="visually-hidden" role="status" aria-live="polite" id="workbench-live">
        {workbench.announcement}
      </div>
    </>
  );
}
