// SPDX-License-Identifier: MIT

// Resizing what the viewer sees. A dock library is shown the *visible* tree (collapsed groups removed), and
// removing a group can fold splits together: with the panel area collapsed, the default layout's
// `row[project, column[row[editor, canvas], bottom]]` is `row[project, editor, canvas]` on screen. A sash drag
// or a splitter key therefore reports sizes for the visible split, and this module lifts them back onto the
// real tree, so the real model stays canonical and collapsed groups keep the share they had.
//
// The rule, per real split that the visible split was built from: children that show nothing keep their
// size; the rest share what remains in the proportion of their new visible weights.

import { canonicalSizes, cloneLayout, groupsOf, visibleLayout } from "./tree";
import { resizeSplit } from "./ops";
import type { Layout, LayoutNode, Result, SplitNode } from "./types";
import { diff } from "./tree";

interface Item {
  weight: number;
  groups: Set<string>;
}

function groupIds(node: LayoutNode): Set<string> {
  return new Set(groupsOf(node).map((group) => group.id));
}

function nodeAt(root: LayoutNode, path: readonly number[]): LayoutNode | null {
  let node: LayoutNode = root;
  for (const index of path) {
    if (node.type !== "split" || index < 0 || index >= node.children.length) {
      return null;
    }
    node = node.children[index];
  }
  return node;
}

function lift(split: SplitNode, items: readonly Item[]): void {
  const parts = split.children.map((child) => {
    const ids = groupIds(child);
    const inside = items.filter((item) => [...item.groups].every((id) => ids.has(id)));
    return { child, inside, weight: inside.reduce((sum, item) => sum + item.weight, 0) };
  });
  const removed = parts.reduce((sum, part, index) => (part.weight === 0 ? sum + split.sizes[index] : sum), 0);
  const weights = parts.reduce((sum, part) => sum + part.weight, 0);
  if (weights <= 0) {
    return;
  }
  split.sizes = canonicalSizes(
    parts.map((part, index) => (part.weight === 0 ? split.sizes[index] : ((1 - removed) * part.weight) / weights)),
  );
  for (const part of parts) {
    if (part.inside.length >= 2 && part.child.type === "split") {
      lift(part.child, part.inside);
    }
  }
}

/** `path` and `sizes` describe a split of the visible tree; the returned layout has the real tree resized. */
export function resizeVisibleSplit(layout: Layout, path: readonly number[], sizes: readonly number[]): Result {
  const unchanged: Result = { layout, change: diff(layout, layout) };
  if (layout.collapsed.length === 0) {
    return resizeSplit(layout, path, sizes);
  }
  const visible = nodeAt(visibleLayout(layout).root, path);
  if (
    visible === null ||
    visible.type !== "split" ||
    sizes.length !== visible.children.length ||
    !sizes.every((size) => Number.isFinite(size) && size > 0)
  ) {
    return unchanged;
  }
  const total = sizes.reduce((sum, size) => sum + size, 0);
  const items: Item[] = visible.children.map((child, index) => ({ weight: sizes[index] / total, groups: groupIds(child) }));
  const wanted = new Set(items.flatMap((item) => [...item.groups]));
  const next = cloneLayout(layout);
  // The real split this visible one is made of: the deepest real split that still holds every group of it.
  let real: LayoutNode = next.root;
  for (;;) {
    if (real.type !== "split") {
      return unchanged;
    }
    const holder: LayoutNode | undefined = real.children.find((child) => {
      const ids = groupIds(child);
      return [...wanted].every((id) => ids.has(id));
    });
    if (holder === undefined || holder.type !== "split") {
      break;
    }
    real = holder;
  }
  if (real.type !== "split" || real.dir !== visible.dir) {
    return unchanged;
  }
  lift(real, items);
  return { layout: next, change: diff(layout, next) };
}
