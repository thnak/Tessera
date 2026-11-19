// SPDX-License-Identifier: MIT

// The layout store and the plain command functions (`view.*`, `layout.reset`). The registry (W1) calls these
// and nothing else; the dock adapter calls `dispatch`. No React: a React hook subscribes through `subscribe`.

import { defaultContext } from "./context";
import { PANEL_AREA_GROUP, directionTarget, type Direction } from "./keyboard";
import { applyOperation, resetLayout } from "./ops";
import { findGroup, groupOfPanel, visiblePanels } from "./tree";
import type { Context, Layout, Operation, Result } from "./types";

export interface LayoutStore {
  get(): Layout;
  /** Applies an operation; listeners run only when the layout actually changed. */
  dispatch(operation: Operation): Result;
  /** Replaces the layout wholesale (a stored layout loaded at boot). */
  set(layout: Layout): void;
  subscribe(listener: (result: Result) => void): () => void;
}

export function createLayoutStore(initial: Layout, ctx: Context = defaultContext): LayoutStore {
  let layout = initial;
  const listeners = new Set<(result: Result) => void>();
  return {
    get: () => layout,
    dispatch(operation) {
      const result = applyOperation(layout, operation, ctx);
      if (result.layout !== layout) {
        layout = result.layout;
        for (const listener of [...listeners]) {
          listener(result);
        }
      }
      return result;
    },
    set(next) {
      layout = next;
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => void listeners.delete(listener);
    },
  };
}

/** The group `view.togglePanelArea` shows and hides: `g-bottom`, else whichever group holds Diagnostics. */
export function panelAreaGroup(layout: Layout): string | null {
  return findGroup(layout, PANEL_AREA_GROUP)?.id ?? groupOfPanel(layout, "diagnostics")?.id ?? null;
}

/** Whether the panel area is open (exists and not collapsed). */
export function panelAreaOpen(layout: Layout): boolean {
  const id = panelAreaGroup(layout);
  return id !== null && !layout.collapsed.includes(id);
}

/**
 * The command surface over a store, as plain functions: the registry binds `view.togglePanel`,
 * `view.togglePanelArea`, `view.movePanel`, `view.maximizePanel`, `view.layoutPreset` and `layout.reset` to
 * these. Each returns the `Result` so a caller can announce what changed.
 */
export interface WorkbenchCommands {
  /** Open if closed, reveal if hidden, close if visible (VS Code's toggle). */
  togglePanel(panelId: string): Result;
  togglePanelArea(): Result;
  /** Open (or reveal) the panel area, e.g. on a new diagnostic. */
  revealPanelArea(): Result;
  movePanelDirection(panelId: string, direction: Direction): Result;
  maximizePanel(panelId?: string | null): Result;
  layoutPreset(presetId: string): Result;
  resetLayout(): Result;
  isDefault(): boolean;
}

export function createCommands(store: LayoutStore, ctx: Context = defaultContext): WorkbenchCommands {
  const run = (operation: Operation): Result => store.dispatch(operation);
  return {
    togglePanel(panelId) {
      const layout = store.get();
      if (groupOfPanel(layout, panelId) === null) {
        return run({ kind: "open", panelId });
      }
      return visiblePanels(layout).has(panelId) ? run({ kind: "close", panelId }) : run({ kind: "activate", panelId });
    },
    togglePanelArea() {
      const layout = store.get();
      const id = panelAreaGroup(layout);
      if (id === null) {
        return run({ kind: "open", panelId: "diagnostics" });
      }
      return run({ kind: "collapse", groupId: id, collapsed: !layout.collapsed.includes(id) });
    },
    revealPanelArea() {
      const layout = store.get();
      const id = panelAreaGroup(layout);
      if (id === null) {
        return run({ kind: "open", panelId: "diagnostics" });
      }
      return run({ kind: "collapse", groupId: id, collapsed: false });
    },
    movePanelDirection(panelId, direction) {
      const target = directionTarget(store.get(), panelId, direction);
      return target === null ? run({ kind: "activate", panelId }) : run({ kind: "move", panelId, target });
    },
    maximizePanel(panelId) {
      const layout = store.get();
      if (panelId === undefined) {
        return run({ kind: "maximize", panelId: layout.maximized === null ? activeOfFocus(layout) : null });
      }
      return run({ kind: "maximize", panelId });
    },
    layoutPreset: (presetId) => run({ kind: "preset", presetId }),
    resetLayout: () => run({ kind: "reset" }),
    isDefault: () => JSON.stringify(store.get()) === JSON.stringify(resetLayout(store.get(), ctx).layout),
  };
}

function activeOfFocus(layout: Layout): string | null {
  const shown = [...visiblePanels(layout)];
  return shown.includes("canvas") ? "canvas" : (shown[0] ?? null);
}
