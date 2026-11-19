// SPDX-License-Identifier: MIT

// Library gestures to model operations (studio-workbench-layout-v1.md section 1.4). The dock library is
// asked, through `onAction`, before it changes anything; this module says what the gesture means in
// Tessera's own terms and the adapter then *cancels* the library's change: the model changes, and the
// library is re-rendered from it. The library therefore never owns the layout.
//
// The map is one to one: `selectTab` -> activate, `deleteTab` -> close, `moveNode` -> move,
// `adjustWeights` -> resize, `maximizeToggle` -> maximize. Anything else (popout, float, rename, group)
// is not part of V1 and is refused.

import { Actions, type Action } from "flexlayout-react";
import { groupOfPanel, type Edge, type Layout, type MoveTarget, type Operation } from "../workbench";
import { pathOfRowId } from "./codec";

export type Gesture =
  /** An operation to apply, then re-render from the model. */
  | { kind: "operation"; operation: Operation }
  /** Bookkeeping with no model effect (the active tabset's accent): let the library do it. */
  | { kind: "pass" }
  /** Not a V1 gesture. */
  | { kind: "refuse" };

const EDGE: Record<string, Edge | "center"> = { top: "top", bottom: "bottom", left: "left", right: "right", center: "center" };

/** The target a drop on `toNode` means, in model terms; `null` when the drop point is not a group or the root. */
export function targetOfDrop(layout: Layout, toNode: string, location: string, index: number): MoveTarget | null {
  const place = EDGE[location];
  if (place === undefined) {
    return null;
  }
  const isRoot = toNode.startsWith("row:");
  if (isRoot) {
    const path = pathOfRowId(toNode);
    // An edge dock lands on the top-level row; a nested row is not a drop target of ours.
    return path !== null && path.length === 0 && place !== "center" ? { edge: place, of: "root" } : null;
  }
  if (place === "center") {
    return { group: toNode, index: index < 0 ? undefined : index };
  }
  void layout;
  return { edge: place, of: toNode };
}

export function gestureOf(action: Action, layout: Layout, activePanel: (groupId: string) => string | null): Gesture {
  const data = action.data as Record<string, unknown>;
  switch (action.type) {
    case Actions.SELECT_TAB:
      return { kind: "operation", operation: { kind: "activate", panelId: String(data.tabNode) } };
    case Actions.DELETE_TAB:
      return { kind: "operation", operation: { kind: "close", panelId: String(data.node) } };
    case Actions.MOVE_NODE: {
      const panelId = String(data.fromNode);
      if (groupOfPanel(layout, panelId) === null) {
        return { kind: "refuse" };
      }
      const target = targetOfDrop(layout, String(data.toNode), String(data.location), Number(data.index));
      return target === null ? { kind: "refuse" } : { kind: "operation", operation: { kind: "move", panelId, target } };
    }
    case Actions.ADJUST_WEIGHTS: {
      const path = pathOfRowId(String(data.nodeId));
      const weights = data.weights as number[];
      if (path === null || !Array.isArray(weights)) {
        return { kind: "refuse" };
      }
      const total = weights.reduce((sum, weight) => sum + weight, 0);
      return { kind: "operation", operation: { kind: "resize", path, sizes: weights.map((weight) => weight / total), visible: true } };
    }
    case Actions.MAXIMIZE_TOGGLE: {
      const groupId = String(data.node);
      if (layout.maximized !== null && groupOfPanel(layout, layout.maximized)?.id === groupId) {
        return { kind: "operation", operation: { kind: "maximize", panelId: null } };
      }
      const panelId = activePanel(groupId);
      return panelId === null ? { kind: "refuse" } : { kind: "operation", operation: { kind: "maximize", panelId } };
    }
    case Actions.SET_ACTIVE_TABSET:
      return { kind: "pass" };
    default:
      return { kind: "refuse" };
  }
}
