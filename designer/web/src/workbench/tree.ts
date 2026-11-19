// SPDX-License-Identifier: MIT

// Pure helpers over the layout tree: reading order, lookup, canonical normalization, visibility.

import type { Change, GroupNode, Layout, LayoutNode, SplitNode } from "./types";

export function cloneLayout(layout: Layout): Layout {
  return JSON.parse(JSON.stringify(layout)) as Layout;
}

/** Groups in reading order (left to right, top to bottom): the order DOM and focus follow (spec 4.4). */
export function groupsOf(node: LayoutNode): GroupNode[] {
  if (node.type === "group") {
    return [node];
  }
  return node.children.flatMap(groupsOf);
}

export function findGroup(layout: Layout, id: string): GroupNode | null {
  return groupsOf(layout.root).find((group) => group.id === id) ?? null;
}

export function groupOfPanel(layout: Layout, panelId: string): GroupNode | null {
  return groupsOf(layout.root).find((group) => group.panels.includes(panelId)) ?? null;
}

/** Open panels (in some group) in reading order. */
export function openPanels(layout: Layout): string[] {
  return groupsOf(layout.root).flatMap((group) => group.panels);
}

const EPS = 1e-6;

/** Fractions rounded to 6 places that sum to exactly 1 (the last absorbs the remainder): canonical JSON. */
export function canonicalSizes(raw: readonly number[]): number[] {
  const total = raw.reduce((sum, value) => sum + value, 0);
  const scaled = raw.map((value) => Math.round((value / total) * 1e6) / 1e6);
  const head = scaled.slice(0, -1).reduce((sum, value) => sum + value, 0);
  scaled[scaled.length - 1] = Math.round((1 - head) * 1e6) / 1e6;
  return scaled;
}

/**
 * The canonical form: empty groups are removed, a split with one child is replaced by it, and a split
 * never has a same-direction split child (it is flattened, sizes multiplied). Returns `null` for an
 * empty tree.
 */
export function normalize(node: LayoutNode): LayoutNode | null {
  if (node.type === "group") {
    return node.panels.length > 0 ? node : null;
  }
  const kept: { node: LayoutNode; size: number }[] = [];
  node.children.forEach((child, index) => {
    const next = normalize(child);
    if (next !== null) {
      kept.push({ node: next, size: node.sizes[index] ?? 0 });
    }
  });
  if (kept.length === 0) {
    return null;
  }
  if (kept.length === 1) {
    return kept[0].node;
  }
  const flat: { node: LayoutNode; size: number }[] = [];
  for (const entry of kept) {
    const inner = entry.node;
    if (inner.type === "split" && inner.dir === node.dir) {
      inner.children.forEach((child, index) => {
        flat.push({ node: child, size: entry.size * inner.sizes[index] });
      });
    } else {
      flat.push(entry);
    }
  }
  const out: SplitNode = {
    type: "split",
    dir: node.dir,
    sizes: canonicalSizes(flat.map((entry) => entry.size)),
    children: flat.map((entry) => entry.node),
  };
  return out;
}

/** Drops `collapsed` ids whose group is gone, and a `maximized` panel that is no longer open. */
export function prune(layout: Layout): Layout {
  const ids = new Set(groupsOf(layout.root).map((group) => group.id));
  const open = new Set(openPanels(layout));
  layout.collapsed = layout.collapsed.filter((id) => ids.has(id));
  if (layout.maximized !== null && !open.has(layout.maximized)) {
    layout.maximized = null;
  }
  return layout;
}

/** The panels a viewer can see: the active tab of every expanded group (only one group if maximized). */
export function visiblePanels(layout: Layout): Set<string> {
  const out = new Set<string>();
  const maxGroup = layout.maximized === null ? null : groupOfPanel(layout, layout.maximized);
  for (const group of groupsOf(layout.root)) {
    if (layout.collapsed.includes(group.id)) {
      continue;
    }
    if (maxGroup !== null && group.id !== maxGroup.id) {
      continue;
    }
    out.add(group.active);
  }
  return out;
}

export function diff(before: Layout, after: Layout): Change {
  const openBefore = new Set(openPanels(before));
  const openAfter = new Set(openPanels(after));
  const shownBefore = visiblePanels(before);
  const shownAfter = visiblePanels(after);
  const only = (a: Set<string>, b: Set<string>): string[] => [...a].filter((id) => !b.has(id)).sort();
  return {
    opened: only(openAfter, openBefore),
    closed: only(openBefore, openAfter),
    shown: only(shownAfter, shownBefore),
    hidden: only(shownBefore, shownAfter),
  };
}

/** A unique `g-<slug>` id; minted by the model, never by a library. */
export function mintGroupId(layout: Layout, base: string): string {
  const slug = base.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "group";
  const taken = new Set(groupsOf(layout.root).map((group) => group.id));
  let candidate = `g-${slug}`;
  for (let n = 2; taken.has(candidate); n += 1) {
    candidate = `g-${slug}-${n}`;
  }
  return candidate;
}

export function sumsToOne(sizes: readonly number[]): boolean {
  return Math.abs(sizes.reduce((sum, value) => sum + value, 0) - 1) <= EPS;
}
