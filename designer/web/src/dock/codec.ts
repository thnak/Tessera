// SPDX-License-Identifier: MIT

// The flexlayout-react codec: Tessera's layout model to the library's JSON and back
// (studio-workbench-layout-v1.md section 2.4). The library JSON is derived, never stored, and never read
// back as truth: gestures come back as operations (`actions.ts`), so this direction exists to render and to
// prove the mapping is lossless (a W5 gate round-trips every layout through it).
//
// Three rules shape the mapping.
//
//   * **Collapsed groups are dropped.** A collapsed group is hidden in the model (it keeps its size, panels and
//     tab), but the library never sees it, so a collapsed panel area costs nothing and shows nothing but the
//     status bar item (the owner's VS Code-style rule). `fromLibrary` therefore returns the *visible
//     projection*: equal to `visibleLayout(original)`.
//   * **Ids are ours.** A library tab id is the panel id and a tabset id is the group id; row ids encode the
//     path (`row:0.1`) so a weight change maps back to `resizeSplit(path, ...)`.
//   * **Orientation is by depth.** The library alternates row orientation by depth from a horizontal root
//     (or a vertical one with `rootOrientationVertical`). The model's canonical form never nests a split in
//     a same-direction split, so the two always agree.

import type { IJsonModel, IJsonRowNode, IJsonTabSetNode } from "flexlayout-react";
import {
  canonicalSizes,
  cloneLayout,
  groupsOf,
  normalize,
  visibleLayout,
  type GroupNode,
  type Layout,
  type LayoutNode,
  type PanelDef,
  type SplitNode,
} from "../workbench/layout";

export { visibleLayout };

export const ROW_ID_PREFIX = "row:";

export function rowId(path: readonly number[]): string {
  return `${ROW_ID_PREFIX}${path.join(".")}`;
}

/** The split path a row id names, or `null` when the id is not one of ours. */
export function pathOfRowId(id: string): number[] | null {
  if (!id.startsWith(ROW_ID_PREFIX)) {
    return null;
  }
  const rest = id.slice(ROW_ID_PREFIX.length);
  return rest === "" ? [] : rest.split(".").map(Number);
}

export interface CodecOptions {
  defs: readonly PanelDef[];
  /** The group the viewer last acted in (the active tabset's accent). */
  activeGroup?: string | null;
  /** Phone mode: no drag, no maximize. */
  phone?: boolean;
}

function tabSetJson(group: GroupNode, layout: Layout, options: CodecOptions): IJsonTabSetNode {
  const defs = group.panels.map((id) => options.defs.find((def) => def.id === id));
  const minWidth = Math.max(1, ...defs.map((def) => def?.minSize?.w ?? 1));
  const minHeight = Math.max(1, ...defs.map((def) => def?.minSize?.h ?? 1));
  return {
    type: "tabset",
    id: group.id,
    name: defs.map((def, index) => def?.title ?? group.panels[index]).join(", "),
    selected: group.panels.indexOf(group.active),
    active: options.activeGroup === group.id,
    maximized: layout.maximized !== null && group.panels.includes(layout.maximized),
    minWidth,
    minHeight,
    // The panel area's strip underlines its selected tab (VS Code's bottom panel); editor-like groups
    // overline it. Styling only: the group id decides, nothing reads it back.
    classNameTabStrip: group.id === "g-bottom" ? "tabstrip-panel" : undefined,
    enableMaximize: options.phone !== true,
    enableDrag: options.phone !== true,
    children: group.panels.map((id, index) => ({
      type: "tab" as const,
      id,
      name: defs[index]?.title ?? id,
      component: id,
      enableClose: defs[index]?.closable ?? true,
      enableDrag: options.phone !== true,
    })),
  };
}

function rowJson(node: SplitNode, path: number[], layout: Layout, options: CodecOptions, weight: number): IJsonRowNode {
  return {
    type: "row",
    id: rowId(path),
    weight,
    children: node.children.map((child, index) => childJson(child, [...path, index], layout, options, node.sizes[index] * 100)),
  };
}

function childJson(
  node: LayoutNode,
  path: number[],
  layout: Layout,
  options: CodecOptions,
  weight: number,
): IJsonRowNode | IJsonTabSetNode {
  if (node.type === "group") {
    return { ...tabSetJson(node, layout, options), weight };
  }
  return rowJson(node, path, layout, options, weight);
}

export function toLibrary(layout: Layout, options: CodecOptions): IJsonModel {
  const shown = visibleLayout(layout);
  const root = shown.root;
  const vertical = root.type === "split" && root.dir === "column";
  const top: IJsonRowNode =
    root.type === "split"
      ? { ...rowJson(root, [], shown, options, 100) }
      : { type: "row", id: rowId([]), weight: 100, children: [tabSetJson(root, shown, options)] };
  return {
    global: {
      tabEnableRenderOnDemand: false,
      tabEnablePopout: false,
      tabEnableRename: false,
      tabSetEnableMaximize: options.phone !== true,
      tabSetEnableDrag: false,
      tabSetEnableClose: false,
      enableEdgeDock: true,
      rootOrientationVertical: vertical,
    },
    borders: [],
    layout: top,
  };
}

function nodeFrom(json: IJsonRowNode | IJsonTabSetNode, dir: "row" | "column"): LayoutNode {
  if (json.type === "tabset") {
    const tabset = json as IJsonTabSetNode;
    const panels = (tabset.children ?? []).map((tab) => (tab as { id: string }).id);
    const selected = typeof tabset.selected === "number" && tabset.selected >= 0 ? tabset.selected : 0;
    return { type: "group", id: tabset.id as string, panels, active: panels[selected] ?? panels[0] };
  }
  const children = ((json as IJsonRowNode).children ?? []) as (IJsonRowNode | IJsonTabSetNode)[];
  const inner: "row" | "column" = dir === "row" ? "column" : "row";
  return {
    type: "split",
    dir,
    sizes: canonicalSizes(children.map((child) => child.weight ?? 100)),
    children: children.map((child) => nodeFrom(child, inner)),
  };
}

/** The visible projection of the layout the library JSON describes. `reference` supplies what it cannot hold. */
export function fromLibrary(json: IJsonModel, reference: Layout): Layout {
  const vertical = json.global?.rootOrientationVertical === true;
  const parsed = nodeFrom(json.layout, vertical ? "column" : "row");
  const root = normalize(parsed) as LayoutNode;
  const out = cloneLayout(reference);
  out.root = root;
  out.collapsed = [];
  const maximized = groupsOf(root).find((group) => {
    const flat: IJsonTabSetNode[] = [];
    const collect = (node: IJsonRowNode | IJsonTabSetNode): void => {
      if (node.type === "tabset") {
        flat.push(node);
      } else {
        (node.children ?? []).forEach((child) => collect(child as IJsonRowNode | IJsonTabSetNode));
      }
    };
    collect(json.layout);
    return flat.some((tabset) => tabset.id === group.id && tabset.maximized === true);
  });
  out.maximized = maximized === undefined ? null : maximized.active;
  return out;
}
