// SPDX-License-Identifier: MIT

// The workbench layout model's types (studio-workbench-layout-v1.md section 1). Pure data: no DOM, no
// React, no dock library. A dock library is an adapter that renders this and reports gestures back as
// operations (`ops.ts`).

export const LAYOUT_FORMAT = "tessera.studio.layout";
export const LAYOUT_VERSION = 1;

export type Dir = "row" | "column";
export type Edge = "left" | "right" | "top" | "bottom";

/** An ordered tab strip of panel instances, one active. */
export interface GroupNode {
  type: "group";
  id: string;
  panels: string[];
  active: string;
}

/** A row or column of groups and nested splits with proportional sizes. */
export interface SplitNode {
  type: "split";
  dir: Dir;
  sizes: number[];
  children: LayoutNode[];
}

export type LayoutNode = GroupNode | SplitNode;

export interface Shell {
  activityBar: boolean;
  statusBar: boolean;
  toolbar: boolean;
  sideBar: "left" | "right";
  /** Transient: never written to storage. */
  zen: boolean;
}

export interface Layout {
  format: typeof LAYOUT_FORMAT;
  version: number;
  root: LayoutNode;
  /** `null` or a panel id: its group fills the dock; the others stay mounted. */
  maximized: string | null;
  /** Panels the viewer closed, so a new panel can be told apart from a closed one. */
  closed: string[];
  shell: Shell;
  /** Group ids hidden but keeping their size, panels and active tab. */
  collapsed: string[];
}

/** Where a moved panel goes: a tab in a group, or a new split at an edge of a group (or of the root). */
export type MoveTarget = { group: string; index?: number } | { edge: Edge; of: string };

/** What a panel is (`panels.json`). */
export interface PanelDef {
  id: string;
  title: string;
  icon: string;
  singleton: boolean;
  closable: boolean;
  /** Never offered and never placed until its slice lands. */
  reserved?: boolean;
  defaultLocation?: { group: string };
  minSize?: { w: number; h: number };
  lazyModule?: string;
  requires?: string[];
  drawsWhenHidden?: boolean;
  /** Its DOM must never be detached by a layout change (the Canvas surface). */
  keepMounted?: boolean;
}

/** Which panel instances changed state because of an operation. */
export interface Change {
  opened: string[];
  closed: string[];
  shown: string[];
  hidden: string[];
}

export interface Result {
  layout: Layout;
  change: Change;
}

/** What operations need beyond the layout: the panel definitions, the default layout and the presets. */
export interface Context {
  defs: readonly PanelDef[];
  defaults: Layout;
  presets: Readonly<Record<string, Layout>>;
}

/** A user gesture or command, library independent. `applyOperation` is the single dispatcher. */
export type Operation =
  | { kind: "open"; panelId: string; where?: MoveTarget }
  | { kind: "close"; panelId: string }
  | { kind: "move"; panelId: string; target: MoveTarget }
  | { kind: "activate"; panelId: string }
  | { kind: "split"; groupId: string; dir: Dir; panelId: string }
  /** `visible: true` means `path` is a path in the tree a viewer sees (collapsed groups removed): what a dock reports. */
  | { kind: "resize"; path: number[]; sizes: number[]; visible?: boolean }
  | { kind: "maximize"; panelId: string | null }
  | { kind: "reset" }
  | { kind: "collapse"; groupId: string; collapsed: boolean }
  | { kind: "preset"; presetId: string };

export interface ValidationError {
  /** Spec rule id, e.g. `sizes-sum`. */
  rule: string;
  /** JSON path of the offending value. */
  path: string;
  message: string;
}

export type ValidationResult = { ok: true; layout: Layout } | { ok: false; errors: ValidationError[] };
