// SPDX-License-Identifier: MIT

// Sash behaviors as model operations (studio-workbench-layout-v1.md section 4.4, "Sashes and drop zones"): a
// sash drag **snaps** to the split's default fractions and to a group's `minSize`, dragging a group below
// `minSize` **collapses** it (`collapseGroup`, persisted in the layout JSON), and a double-click **resets** the
// split to its default fractions. The dock library only sees the visible tree and clamps a drag at the
// minimum, so the adapter reports where the pointer put the sash and these functions decide what that means.
// No DOM and no library: the pointer, the keyboard and the gates all end in the same operations.

import { collapseGroup } from "./ops";
import { resizeVisibleSplit } from "./resize";
import { canonicalSizes, groupsOf, visibleLayout } from "./tree";
import type { Context, Layout, LayoutNode, PanelDef, Result, SplitNode } from "./types";

/** A drag that ends this close (CSS px) to a snap point lands on it. */
export const SNAP_PX = 12;
/** A drag that overshoots a group's minimum by more than this (CSS px) collapses the group. */
export const COLLAPSE_PX = 24;
const FALLBACK_MIN_PX = 48;

/** Minimum size of a child in CSS px along the split's axis: the largest `minSize` among its panels. */
export function childMinPx(node: LayoutNode, dir: "row" | "column", defs: readonly PanelDef[]): number {
  if (node.type === "group") {
    return Math.max(
      FALLBACK_MIN_PX,
      ...node.panels.map((id) => {
        const size = defs.find((def) => def.id === id)?.minSize;
        return (dir === "row" ? size?.w : size?.h) ?? FALLBACK_MIN_PX;
      }),
    );
  }
  return FALLBACK_MIN_PX;
}

export function minFractionsOf(split: SplitNode, extentPx: number, defs: readonly PanelDef[]): number[] {
  const fractions = split.children.map((child) => (extentPx > 0 ? childMinPx(child, split.dir, defs) / extentPx : 0.05));
  // Never ask for more than an even share, or a narrow dock could not be resized at all.
  return fractions.map((fraction) => Math.min(fraction, 0.9 / split.children.length));
}

function idsOf(node: LayoutNode): Set<string> {
  return new Set(groupsOf(node).map((group) => group.id));
}

const holdsAll = (node: LayoutNode, ids: ReadonlySet<string>): boolean => {
  const own = idsOf(node);
  return [...ids].every((id) => own.has(id));
};

/**
 * The default share of each item (a set of group ids) inside `node`, summing to 1, or `null` when the default
 * tree cannot say (an item that spans two default children, a group the default layout does not have, an axis
 * that differs). Collapsed groups are not items, so the shares of the groups that remain are rescaled.
 */
function defaultShares(items: readonly ReadonlySet<string>[], node: LayoutNode, dir: string): number[] | null {
  const all = new Set(items.flatMap((item) => [...item]));
  let at = node;
  while (at.type === "split") {
    const holder: LayoutNode | undefined = at.children.find((child) => holdsAll(child, all));
    if (holder === undefined) {
      break;
    }
    at = holder;
  }
  if (at.type === "group") {
    return items.length === 1 ? [1] : null;
  }
  if (at.dir !== dir) {
    return null;
  }
  const slots = items.map((item) => at.children.findIndex((child) => holdsAll(child, item)));
  if (slots.some((slot) => slot < 0)) {
    return null;
  }
  const weights: number[] = new Array(items.length).fill(0);
  for (const slot of new Set(slots)) {
    const members = items.map((_, index) => index).filter((index) => slots[index] === slot);
    const share = at.sizes[slot];
    if (members.length === 1) {
      weights[members[0]] = share;
      continue;
    }
    const inner = defaultShares(members.map((index) => items[index]), at.children[slot], dir);
    if (inner === null) {
      return null;
    }
    members.forEach((index, order) => {
      weights[index] = share * inner[order];
    });
  }
  return canonicalSizes(weights);
}

function visibleSplit(layout: Layout, path: readonly number[]): SplitNode | null {
  let node: LayoutNode = visibleLayout(layout).root;
  for (const index of path) {
    if (node.type !== "split" || index < 0 || index >= node.children.length) {
      return null;
    }
    node = node.children[index];
  }
  return node.type === "split" ? node : null;
}

/** The default fractions of the visible split at `path`, or an even share when the default layout has none. */
export function defaultVisibleSizes(layout: Layout, path: readonly number[], ctx: Context): number[] | null {
  const split = visibleSplit(layout, path);
  if (split === null) {
    return null;
  }
  const shares = defaultShares(split.children.map(idsOf), ctx.defaults.root, split.dir);
  return shares ?? canonicalSizes(split.children.map(() => 1));
}

/** Double-click on a sash: the visible split goes back to its default fractions. */
export function resetSplit(layout: Layout, path: readonly number[], ctx: Context): Result {
  const sizes = defaultVisibleSizes(layout, path, ctx);
  return sizes === null
    ? { layout, change: { opened: [], closed: [], shown: [], hidden: [] } }
    : resizeVisibleSplit(layout, path, sizes);
}

/**
 * A sash released at `position` (a fraction of the visible split's extent, measured from its start) for the
 * splitter after child `index`. Past a group's minimum by more than `COLLAPSE_PX` the group collapses; within
 * `SNAP_PX` of a default boundary or of a minimum the sash snaps to it; otherwise it is clamped and applied.
 */
export function dragSash(
  layout: Layout,
  path: readonly number[],
  index: number,
  position: number,
  extentPx: number,
  ctx: Context,
): Result {
  const split = visibleSplit(layout, path);
  const unchanged: Result = { layout, change: { opened: [], closed: [], shown: [], hidden: [] } };
  if (split === null || extentPx <= 0 || index < 0 || index + 1 >= split.children.length || !Number.isFinite(position)) {
    return unchanged;
  }
  const mins = minFractionsOf(split, extentPx, ctx.defs);
  const before = split.sizes.slice(0, index).reduce((sum, size) => sum + size, 0);
  const pair = split.sizes[index] + split.sizes[index + 1];
  const lo = before + mins[index];
  const hi = Math.max(lo, before + pair - mins[index + 1]);
  const slack = COLLAPSE_PX / extentPx;
  const overshoot = position < lo - slack ? split.children[index] : position > hi + slack ? split.children[index + 1] : null;
  if (overshoot !== null && overshoot.type === "group") {
    const collapsed = collapseGroup(layout, overshoot.id, true);
    if (collapsed.layout !== layout) {
      return collapsed;
    }
  }
  let at = Math.min(hi, Math.max(lo, position));
  const defaults = defaultVisibleSizes(layout, path, ctx);
  const points = [lo, hi];
  if (defaults !== null) {
    const boundary = defaults.slice(0, index + 1).reduce((sum, size) => sum + size, 0);
    if (boundary >= lo && boundary <= hi) {
      points.unshift(boundary);
    }
  }
  const near = points.find((point) => Math.abs(point - at) * extentPx <= SNAP_PX);
  if (near !== undefined) {
    at = near;
  }
  const sizes = [...split.sizes];
  sizes[index] = at - before;
  sizes[index + 1] = pair - sizes[index];
  return resizeVisibleSplit(layout, path, sizes);
}

/** Where a collapsed group's restore handle sits: on the `edge` of the visible groups `beside` it. */
export interface CollapsedEdge {
  groupId: string;
  /** The axis of the split the group is in: `column` puts the handle on a horizontal edge. */
  dir: "row" | "column";
  beside: string[];
  /** `end` is the far edge (right or bottom) of `beside`; `start` the near edge. */
  edge: "start" | "end";
}

function edgeOf(node: LayoutNode, layout: Layout, id: string): CollapsedEdge | null {
  if (node.type === "group") {
    return null;
  }
  const at = node.children.findIndex((child) => idsOf(child).has(id));
  if (at < 0) {
    return null;
  }
  const child = node.children[at];
  if (child.type === "split") {
    const inner = edgeOf(child, layout, id);
    if (inner !== null) {
      return inner;
    }
  }
  const shown = (sibling: LayoutNode): string[] =>
    groupsOf(sibling)
      .map((group) => group.id)
      .filter((groupId) => !layout.collapsed.includes(groupId));
  for (const [sibling, edge] of [
    [node.children[at - 1], "end"],
    [node.children[at + 1], "start"],
  ] as const) {
    if (sibling !== undefined && shown(sibling).length > 0) {
      return { groupId: id, dir: node.dir, beside: shown(sibling), edge };
    }
  }
  return null;
}

/** One restore handle per collapsed group that has a visible neighbour in its own split. */
export function collapsedEdges(layout: Layout): CollapsedEdge[] {
  return layout.collapsed.flatMap((id) => {
    const edge = edgeOf(layout.root, layout, id);
    return edge === null ? [] : [edge];
  });
}
