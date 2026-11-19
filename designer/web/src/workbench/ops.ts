// SPDX-License-Identifier: MIT

// The layout operations (studio-workbench-layout-v1.md section 1.4): pure functions returning a new layout
// plus a `change` that says which panel instances changed state. Drag and drop, menu items, palette
// commands and keyboard moves all end here, so every one is testable without a browser.
//
// An operation that cannot apply (unknown panel, a non-closable panel, a target that is gone) returns the
// layout unchanged with an empty change; it never throws.

import { defaultContext } from "./context";
import { defaultTarget, detachPanel, finish, insertPanel } from "./place";
import { reconcile } from "./reconcile";
import { resizeVisibleSplit } from "./resize";
import {
  canonicalSizes,
  cloneLayout,
  diff,
  findGroup,
  groupOfPanel,
  groupsOf,
  sumsToOne,
} from "./tree";
import type { Context, Dir, Layout, MoveTarget, Operation, Result, SplitNode } from "./types";

function result(before: Layout, after: Layout): Result {
  return { layout: after, change: diff(before, after) };
}

const unchanged = (layout: Layout): Result => result(layout, layout);

export function activatePanel(layout: Layout, panelId: string): Result {
  const group = groupOfPanel(layout, panelId);
  if (group === null) {
    return unchanged(layout);
  }
  const next = cloneLayout(layout);
  const target = findGroup(next, group.id) as NonNullable<ReturnType<typeof findGroup>>;
  target.active = panelId;
  next.collapsed = next.collapsed.filter((id) => id !== target.id);
  if (next.maximized !== null && !target.panels.includes(next.maximized)) {
    next.maximized = null;
  } else if (next.maximized !== null) {
    next.maximized = panelId;
  }
  return result(layout, next);
}

export function openPanel(layout: Layout, panelId: string, where?: MoveTarget, ctx: Context = defaultContext): Result {
  if (groupOfPanel(layout, panelId) !== null) {
    return activatePanel(layout, panelId);
  }
  const def = ctx.defs.find((entry) => entry.id === panelId);
  if (def === undefined || def.reserved === true) {
    return unchanged(layout);
  }
  const next = cloneLayout(layout);
  next.closed = next.closed.filter((id) => id !== panelId);
  if (!insertPanel(next, panelId, where ?? defaultTarget(next, panelId, ctx))) {
    insertPanel(next, panelId, defaultTarget(next, panelId, ctx));
  }
  return result(layout, finish(next));
}

export function closePanel(layout: Layout, panelId: string, ctx: Context = defaultContext): Result {
  const def = ctx.defs.find((entry) => entry.id === panelId);
  if (groupOfPanel(layout, panelId) === null || def?.closable === false) {
    return unchanged(layout);
  }
  if (groupsOf(layout.root).reduce((count, group) => count + group.panels.length, 0) <= 1) {
    return unchanged(layout);
  }
  const next = cloneLayout(layout);
  detachPanel(next, panelId);
  if (!next.closed.includes(panelId)) {
    next.closed.push(panelId);
  }
  return result(layout, finish(next));
}

export function movePanel(layout: Layout, panelId: string, target: MoveTarget): Result {
  const source = groupOfPanel(layout, panelId);
  if (source === null) {
    return unchanged(layout);
  }
  if ("group" in target) {
    if (findGroup(layout, target.group) === null) {
      return unchanged(layout);
    }
  } else {
    const anchor = target.of === "root" ? null : findGroup(layout, target.of);
    if (target.of !== "root" && anchor === null) {
      return unchanged(layout);
    }
    // Splitting a group around its only panel changes nothing.
    if (anchor !== null && anchor.id === source.id && source.panels.length === 1) {
      return unchanged(layout);
    }
  }
  const next = cloneLayout(layout);
  const from = findGroup(next, source.id) as NonNullable<ReturnType<typeof findGroup>>;
  let index = "group" in target ? target.index : undefined;
  if ("group" in target && target.group === from.id && index !== undefined) {
    // Reordering inside a group: the index is in terms of the list before the panel is lifted out.
    const at = from.panels.indexOf(panelId);
    if (index > at) {
      index -= 1;
    }
  }
  detachPanel(next, panelId);
  const placed = "group" in target ? insertPanel(next, panelId, { group: target.group, index }) : insertPanel(next, panelId, target);
  if (!placed) {
    return unchanged(layout);
  }
  next.maximized = null;
  return result(layout, finish(next));
}

export function splitGroup(layout: Layout, groupId: string, dir: Dir, panelId: string): Result {
  return movePanel(layout, panelId, { edge: dir === "row" ? "right" : "bottom", of: groupId });
}

function splitAt(layout: Layout, path: readonly number[]): SplitNode | null {
  let node = layout.root;
  for (const index of path) {
    if (node.type !== "split" || index < 0 || index >= node.children.length) {
      return null;
    }
    node = node.children[index];
  }
  return node.type === "split" ? node : null;
}

export function resizeSplit(layout: Layout, path: readonly number[], sizes: readonly number[]): Result {
  const probe = splitAt(layout, path);
  if (
    probe === null ||
    sizes.length !== probe.children.length ||
    !sizes.every((size) => Number.isFinite(size) && size > 0) ||
    !sumsToOne(canonicalSizes(sizes))
  ) {
    return unchanged(layout);
  }
  const next = cloneLayout(layout);
  (splitAt(next, path) as SplitNode).sizes = canonicalSizes(sizes);
  return result(layout, next);
}

export function maximize(layout: Layout, panelId: string | null): Result {
  if (panelId === null) {
    if (layout.maximized === null) {
      return unchanged(layout);
    }
    const next = cloneLayout(layout);
    next.maximized = null;
    return result(layout, next);
  }
  const activated = activatePanel(layout, panelId);
  if (groupOfPanel(layout, panelId) === null) {
    return activated;
  }
  const next = cloneLayout(activated.layout);
  next.maximized = panelId;
  return result(layout, next);
}

export function collapseGroup(layout: Layout, groupId: string, collapsed: boolean): Result {
  const group = findGroup(layout, groupId);
  if (group === null || layout.collapsed.includes(groupId) === collapsed) {
    return unchanged(layout);
  }
  if (collapsed && layout.collapsed.length + 1 >= groupsOf(layout.root).length) {
    return unchanged(layout);
  }
  const next = cloneLayout(layout);
  if (collapsed) {
    next.collapsed.push(groupId);
    if (next.maximized !== null && group.panels.includes(next.maximized)) {
      next.maximized = null;
    }
  } else {
    next.collapsed = next.collapsed.filter((id) => id !== groupId);
  }
  return result(layout, next);
}

/** Replaces the layout with the default (reconciled); keeps the transient `zen` flag out of it. */
export function resetLayout(layout: Layout, ctx: Context = defaultContext): Result {
  return result(layout, reconcile(cloneLayout(ctx.defaults), ctx));
}

/** Applies a preset through reconcile, like a stored layout. The caller keeps `layout` for one undo step. */
export function applyPreset(layout: Layout, presetId: string, ctx: Context = defaultContext): Result {
  const preset = presetId === "default" ? ctx.defaults : ctx.presets[presetId];
  if (preset === undefined) {
    return unchanged(layout);
  }
  return result(layout, reconcile(cloneLayout(preset), ctx));
}

/** The single dispatcher: adapters and the command registry call this with a gesture or command. */
export function applyOperation(layout: Layout, op: Operation, ctx: Context = defaultContext): Result {
  switch (op.kind) {
    case "open":
      return openPanel(layout, op.panelId, op.where, ctx);
    case "close":
      return closePanel(layout, op.panelId, ctx);
    case "move":
      return movePanel(layout, op.panelId, op.target);
    case "activate":
      return activatePanel(layout, op.panelId);
    case "split":
      return splitGroup(layout, op.groupId, op.dir, op.panelId);
    case "resize":
      return op.visible === true ? resizeVisibleSplit(layout, op.path, op.sizes) : resizeSplit(layout, op.path, op.sizes);
    case "maximize":
      return maximize(layout, op.panelId);
    case "reset":
      return resetLayout(layout, ctx);
    case "collapse":
      return collapseGroup(layout, op.groupId, op.collapsed);
    case "preset":
      return applyPreset(layout, op.presetId, ctx);
  }
}
