// SPDX-License-Identifier: MIT

// The dock-owned commands (`view.toggleSideBar`, `view.layoutPreset`, `view.movePanel`, ...), the context keys they
// publish, and the menu providers that list panels and groups. This is the join between the command registry
// (`workbench/registry.js`) and the layout store (`workbench/store.ts`): every handler here is one call into the
// store's plain functions, so a menu item, a chord, a palette entry and a tab context menu run the same code.
//
// Nothing here owns state except the one-step undo of a preset or reset, which is a layout identity: the undo
// is available exactly while the layout is still the object that preset or reset produced.

import { useEffect } from "react";
import type { CommandArgs, ContextRecord, MenuProviders, MenuTarget, NotificationInput, Notifications } from "./workbench/index.js";
import {
  PANELS,
  applyPreset,
  findGroup,
  groupChoices,
  moveMessage,
  panelAreaOpen,
  sideBarOpen,
  type Direction,
  type Layout,
} from "./workbench/layout";
import type { Workbench as Dock } from "./dock/use-workbench";
import { focusRegion } from "./page-focus-region";

const PRESET_IDS = ["default", "editor-only", "preview-focus", "side-by-side"] as const;
const PRESET_TITLES: Readonly<Record<string, string>> = {
  default: "Default",
  "editor-only": "Editor Only",
  "preview-focus": "Preview Focus",
  "side-by-side": "Side by Side",
};
const DIRECTION_TITLES: Readonly<Record<Direction, string>> = { left: "Left", right: "Right", top: "Top", bottom: "Bottom" };

/** The layout a preset or reset replaced, and the layout it produced. */
export interface LayoutUndo {
  before: Layout;
  after: Layout;
}

export interface LayoutHost {
  dock: () => Dock;
  notify: (input: NotificationInput) => number | null;
  undo: { current: LayoutUndo | null };
  /** Whether this session has shown the zen-mode exit hint (it is shown once, then lives in the bell history). */
  zenHinted: { current: boolean };
}

/** What entering zen mode tells the person, because the menu bar that could say it is hidden in zen. */
export const ZEN_HINT = "Zen mode: press Esc twice or F1 and choose Exit Zen Mode to leave";

const titleOf = (id: string): string => PANELS.find((def) => def.id === id)?.title ?? id;
const known = (id: string): boolean => PANELS.some((def) => def.id === id && def.reserved !== true);

/** The panel the viewer is working in: the active panel of the group they last acted in, else the editor. */
export function focusedPanel(dock: Dock): string {
  const group = dock.activeGroup === null ? null : findGroup(dock.layout, dock.activeGroup);
  return group?.active ?? "editor";
}

function presetOf(layout: Layout): string {
  const text = JSON.stringify(layout);
  return PRESET_IDS.find((id) => JSON.stringify(applyPreset(layout, id).layout) === text) ?? "";
}

/** Context keys (`studio-workbench-v1.md` section 2.2) derived from the dock, republished after each render. */
export function layoutContext(dock: Dock, undo: LayoutUndo | null): ContextRecord {
  const { layout } = dock;
  const record: ContextRecord = {
    "layout.sideBar": dock.phone ? dock.visible.has("project") : sideBarOpen(layout),
    "layout.panelArea": dock.phone ? dock.visible.has("diagnostics") : panelAreaOpen(layout),
    "layout.statusBar": layout.shell.statusBar,
    "layout.zen": layout.shell.zen,
    "layout.maximized": layout.maximized !== null,
    "layout.isDefault": dock.commands.isDefault(),
    "layout.canUndo": undo !== null && undo.after === layout,
    "layout.preset": presetOf(layout),
  };
  for (const def of PANELS) {
    record[`panel.${def.id}.open`] = dock.open.has(def.id);
    record[`panel.${def.id}.visible`] = dock.visible.has(def.id);
  }
  return record;
}

/** Opens a panel and makes it the shown one: the phone's version of "reveal", where one panel shows at a time. */
function reveal(dock: Dock, panelId: string): void {
  dock.store.dispatch({ kind: "open", panelId });
  dock.dispatch({ kind: "activate", panelId });
}

/** The handlers for every dock-owned command id. */
export function layoutHandlers(host: LayoutHost): Record<string, (args: CommandArgs) => unknown> {
  const dock = (): Dock => host.dock();
  const changed = (before: Layout, after: Layout): boolean => before !== after;

  /** Applies a whole-layout replacement and offers one undo step through the notification service. */
  const replaced = (message: string, apply: (d: Dock) => { layout: Layout }): void => {
    const d = dock();
    const before = d.store.get();
    const result = apply(d);
    if (changed(before, result.layout)) {
      host.undo.current = { before, after: result.layout };
      host.notify({ message, key: "layout-change", actions: [{ label: "Undo layout change", command: "view.undoLayoutChange" }] });
    }
  };

  /** Phone: one panel shows at a time, so "toggle" is show this one or go back to the editor. */
  const phoneToggle = (d: Dock, panelId: string): void => {
    if (d.visible.has(panelId)) {
      d.dispatch({ kind: "activate", panelId: "editor" });
    } else {
      reveal(d, panelId);
    }
  };

  return {
    "view.toggleSideBar": () => {
      const d = dock();
      return d.phone ? phoneToggle(d, "project") : d.commands.toggleSideBar();
    },
    "view.togglePanelArea": () => {
      const d = dock();
      return d.phone ? phoneToggle(d, "diagnostics") : d.commands.togglePanelArea();
    },
    "view.toggleStatusBar": () => {
      const d = dock();
      return d.commands.setStatusBar(!d.layout.shell.statusBar);
    },
    "view.zenMode": () => {
      const d = dock();
      const entering = !d.layout.shell.zen;
      const result = d.commands.setZen(entering);
      if (entering && !host.zenHinted.current) {
        host.zenHinted.current = true;
        host.notify({ message: ZEN_HINT, key: "zen-hint", actions: [{ label: "Exit Zen Mode", command: "view.exitZenMode" }] });
      }
      return result;
    },
    "view.exitZenMode": () => dock().commands.setZen(false),
    "view.layoutPreset": (args) => {
      const id = String(args.preset);
      replaced(`Layout: ${PRESET_TITLES[id] ?? id}`, (d) => d.commands.layoutPreset(id));
    },
    "view.resetLayout": () => replaced("Layout reset to the default", (d) => d.commands.resetLayout()),
    "view.undoLayoutChange": () => {
      const d = dock();
      const undo = host.undo.current;
      if (undo === null || undo.after !== d.store.get()) {
        throw new Error("The layout has changed since; there is nothing to undo.");
      }
      host.undo.current = null;
      d.store.restore(undo.before);
    },
    "view.togglePanel": (args) => {
      const d = dock();
      const id = String(args.id);
      if (!known(id)) {
        throw new Error(`There is no panel '${id}'.`);
      }
      if (args.close === true) {
        return d.dispatch({ kind: "close", panelId: id });
      }
      return d.phone ? phoneToggle(d, id) : d.commands.togglePanel(id);
    },
    "view.maximizePanel": (args) => {
      const d = dock();
      const id = args.id === undefined ? focusedPanel(d) : String(args.id);
      d.dispatch({ kind: "maximize", panelId: d.layout.maximized === null || d.layout.maximized !== id ? id : null });
    },
    "view.movePanel": (args) => {
      const d = dock();
      const id = String(args.id);
      const to = String(args.to) as Direction;
      if (!known(id) || DIRECTION_TITLES[to] === undefined) {
        throw new Error(`Cannot move '${id}' to '${String(args.to)}'.`);
      }
      const before = d.store.get();
      const moved = d.commands.movePanelDirection(id, to).layout !== before;
      d.announce(moved ? moveMessage(titleOf(id), to) : `${titleOf(id)} is already at the ${to}`);
    },
    "view.movePanelToGroup": (args) => {
      const d = dock();
      const id = String(args.id);
      const group = String(args.group);
      if (!known(id) || findGroup(d.layout, group) === null) {
        throw new Error(`Cannot move '${id}' into group '${group}'.`);
      }
      d.commands.movePanelToGroup(id, group);
      d.announce(`${titleOf(id)} moved into the ${titleOf(findGroup(d.store.get(), group)?.active ?? group)} group`);
    },
    "view.focusNextRegion": () => focusRegion(1),
    "view.focusPreviousRegion": () => focusRegion(-1),
  };
}

/** A stored layout set aside at boot (a newer studio wrote it, or it did not validate) is a warning notification. */
export function useLayoutNotices(notice: string | null, notifications: Notifications): void {
  useEffect(() => {
    if (notice !== null) {
      notifications.notify({ message: notice, severity: "warning", key: "layout-notice" });
    }
  }, [notice, notifications]);
}

/** The menu providers: the panel list (checked per panel), the panel-tab menu's entries, and the group list. */
export function layoutProviders(getDock: () => Dock): MenuProviders {
  const panelOf = (target: MenuTarget | undefined): string => target?.panelId ?? focusedPanel(getDock());
  return {
    panels: () => {
      const d = getDock();
      return PANELS.filter((def) => def.reserved !== true).map((def) => ({ label: def.title, args: { id: def.id }, checked: d.visible.has(def.id) }));
    },
    groups: () =>
      groupChoices(getDock().layout, "", PANELS).map((choice) => ({ label: `${choice.label} group`, args: { group: choice.groupId } })),
    tabMaximize: (target) => {
      const d = getDock();
      const id = panelOf(target);
      return [{ label: d.layout.maximized === id ? "Restore Panel" : "Maximize Panel", args: { id } }];
    },
    tabMoves: (target) => {
      const id = panelOf(target);
      return (Object.keys(DIRECTION_TITLES) as Direction[]).map((to) => ({ label: `Move ${DIRECTION_TITLES[to]}`, args: { id, to } }));
    },
    tabMoveGroups: (target) => {
      const id = panelOf(target);
      return groupChoices(getDock().layout, id, PANELS).map((choice) => ({ label: `${choice.label} group`, args: { id, group: choice.groupId } }));
    },
    tabClose: (target) => {
      const id = panelOf(target);
      const def = PANELS.find((entry) => entry.id === id);
      return def !== undefined && def.closable && getDock().open.has(id) ? [{ label: "Close Panel", args: { id, close: true } }] : [];
    },
  };
}
