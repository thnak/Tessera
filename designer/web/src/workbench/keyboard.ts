// SPDX-License-Identifier: MIT

// Keyboard panel moves (studio-workbench-layout-v1.md section 4.4: "Keyboard panel moves are commands, not a
// mouse-only gesture"). Pure: these only choose a `MoveTarget`; `movePanel` does the move, so a keyboard move
// and a drag end in the same operation.

import { findGroup, groupOfPanel, groupsOf } from "./tree";
import type { Edge, Layout, MoveTarget, PanelDef } from "./types";

export type Direction = Edge;

/** The panel-area group VS Code's "Move Panel to Bottom" lands in: the default layout's `g-bottom`. */
export const PANEL_AREA_GROUP = "g-bottom";

function atRootEdge(layout: Layout, groupId: string, edge: Edge): boolean {
  const root = layout.root;
  if (root.type === "group") {
    return true;
  }
  const axis = edge === "left" || edge === "right" ? "row" : "column";
  if (root.dir !== axis) {
    return false;
  }
  const child = edge === "left" || edge === "top" ? root.children[0] : root.children[root.children.length - 1];
  return child.type === "group" && child.id === groupId;
}

/**
 * Where `Alt+Shift+<Arrow>` sends a panel. Down joins the panel area (so a collapsed one opens, like
 * VS Code); the other directions make a new column or row at that edge of the dock. `null` means the panel
 * is already there, so nothing moves.
 */
export function directionTarget(layout: Layout, panelId: string, direction: Direction): MoveTarget | null {
  const source = groupOfPanel(layout, panelId);
  if (source === null) {
    return null;
  }
  if (direction === "bottom" && findGroup(layout, PANEL_AREA_GROUP) !== null) {
    return source.id === PANEL_AREA_GROUP ? null : { group: PANEL_AREA_GROUP };
  }
  if (source.panels.length === 1 && atRootEdge(layout, source.id, direction)) {
    return null;
  }
  return { edge: direction, of: "root" };
}

export interface GroupChoice {
  groupId: string;
  /** The group's active panel's title: how the "Into group" submenu names a group. */
  label: string;
}

/** The "Into group >" submenu: every other group, named by its active panel. */
export function groupChoices(layout: Layout, panelId: string, defs: readonly PanelDef[]): GroupChoice[] {
  const source = groupOfPanel(layout, panelId);
  return groupsOf(layout.root)
    .filter((group) => group.id !== source?.id)
    .map((group) => ({
      groupId: group.id,
      label: defs.find((def) => def.id === group.active)?.title ?? group.active,
    }));
}

const WORDS: Record<Edge, string> = { left: "the left", right: "the right", top: "the top", bottom: "the bottom" };

/** The `aria-live` sentence after a move ("Canvas moved to the bottom"). */
export function moveMessage(title: string, direction: Direction): string {
  return `${title} moved to ${WORDS[direction]}`;
}
