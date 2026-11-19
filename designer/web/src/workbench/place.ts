// SPDX-License-Identifier: MIT

// Panel placement: inserting a panel into the tree and choosing where a new one goes. Shared by
// `openPanel`, `movePanel` and `reconcile`; all of them end in `insertPanel`.

import {
  findGroup,
  groupOfPanel,
  groupsOf,
  mintGroupId,
  normalize,
  prune,
} from "./tree";
import type { Context, GroupNode, Layout, LayoutNode, MoveTarget, SplitNode } from "./types";

const SIDE_FRACTION = 0.3;

/** Removes `panelId` from its group (fixing that group's active tab); an emptied group is normalized away. */
export function detachPanel(layout: Layout, panelId: string): void {
  const group = groupOfPanel(layout, panelId);
  if (group === null) {
    return;
  }
  const index = group.panels.indexOf(panelId);
  group.panels.splice(index, 1);
  if (group.active === panelId && group.panels.length > 0) {
    group.active = group.panels[Math.min(index, group.panels.length - 1)];
  }
}

function replaceNode(root: LayoutNode, target: LayoutNode, replacement: LayoutNode): LayoutNode {
  if (root === target) {
    return replacement;
  }
  if (root.type === "split") {
    root.children = root.children.map((child) => replaceNode(child, target, replacement));
  }
  return root;
}

/**
 * Inserts `panelId` (already detached) at `target`. A tab target joins the group and activates the panel;
 * an edge target makes a new group in a new split. Returns false when the target does not exist.
 */
export function insertPanel(layout: Layout, panelId: string, target: MoveTarget): boolean {
  if ("group" in target) {
    const group = findGroup(layout, target.group);
    if (group === null) {
      return false;
    }
    const at = target.index === undefined ? group.panels.length : Math.max(0, Math.min(target.index, group.panels.length));
    group.panels.splice(at, 0, panelId);
    group.active = panelId;
    layout.collapsed = layout.collapsed.filter((id) => id !== group.id);
    return true;
  }
  const fresh: GroupNode = { type: "group", id: mintGroupId(layout, panelId), panels: [panelId], active: panelId };
  const dir = target.edge === "left" || target.edge === "right" ? "row" : "column";
  const before = target.edge === "left" || target.edge === "top";
  const anchor = target.of === "root" ? layout.root : findGroup(layout, target.of);
  if (anchor === null) {
    return false;
  }
  const share = target.of === "root" ? SIDE_FRACTION : 0.5;
  const split: SplitNode = {
    type: "split",
    dir,
    sizes: before ? [share, 1 - share] : [1 - share, share],
    children: before ? [fresh, anchor] : [anchor, fresh],
  };
  const next = normalize(replaceNode(layout.root, anchor, split));
  if (next === null) {
    return false;
  }
  layout.root = next;
  return true;
}

/** Where a panel with no stated location goes: its default group, the default layout's group, else the last group. */
export function defaultTarget(layout: Layout, panelId: string, ctx: Context): MoveTarget {
  const def = ctx.defs.find((entry) => entry.id === panelId);
  if (def?.defaultLocation !== undefined) {
    const wanted = `g-${def.defaultLocation.group}`;
    if (findGroup(layout, wanted) !== null) {
      return { group: wanted };
    }
  }
  const inDefault = groupOfPanel(ctx.defaults, panelId);
  if (inDefault !== null && findGroup(layout, inDefault.id) !== null) {
    return { group: inDefault.id };
  }
  const all = groupsOf(layout.root);
  return { group: all[all.length - 1].id };
}

export function finish(layout: Layout): Layout {
  const root = normalize(layout.root);
  if (root !== null) {
    layout.root = root;
  }
  return prune(layout);
}
