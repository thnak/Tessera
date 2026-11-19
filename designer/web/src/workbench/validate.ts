// SPDX-License-Identifier: MIT

// `validateLayout`: the hand-written validator (no schema library) for every rule of
// studio-workbench-layout-v1.md section 1.3. It checks the *shape and invariants* of a document; whether a
// panel id is still defined is `reconcile`'s job (section 2.3), so an unknown id is valid here.

import { sumsToOne } from "./tree";
import {
  LAYOUT_FORMAT,
  type Layout,
  type LayoutNode,
  type ValidationError,
  type ValidationResult,
} from "./types";

export const MAX_LAYOUT_BYTES = 64 * 1024;
export const MAX_DEPTH = 8;
const GROUP_ID = /^g-[a-z0-9]+(?:-[a-z0-9]+)*$/;
const PANEL_ID = /^[a-z][a-z0-9-]*$/;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function validateLayout(doc: unknown): ValidationResult {
  const errors: ValidationError[] = [];
  const add = (rule: string, path: string, message: string): void => {
    errors.push({ rule, path, message });
  };

  if (!isObject(doc)) {
    return { ok: false, errors: [{ rule: "document", path: "$", message: "the layout is not an object" }] };
  }
  let bytes = 0;
  try {
    bytes = JSON.stringify(doc).length;
  } catch {
    add("document", "$", "the layout is not serializable");
  }
  if (bytes > MAX_LAYOUT_BYTES) {
    add("size-limit", "$", `the layout is ${bytes} bytes; the limit is ${MAX_LAYOUT_BYTES}`);
  }
  if (doc.format !== LAYOUT_FORMAT) {
    add("format", "$.format", `format must be "${LAYOUT_FORMAT}"`);
  }
  if (typeof doc.version !== "number" || !Number.isInteger(doc.version) || doc.version < 1) {
    add("version", "$.version", "version must be a positive integer");
  }

  const groupIds = new Set<string>();
  const seenPanels = new Set<string>();
  const claimPanel = (id: unknown, path: string): void => {
    if (typeof id !== "string" || !PANEL_ID.test(id)) {
      add("panel-id", path, "a panel id is a lowercase kebab string");
      return;
    }
    if (seenPanels.has(id)) {
      add("panel-once", path, `panel "${id}" appears more than once`);
    }
    seenPanels.add(id);
  };

  const walk = (node: unknown, path: string, depth: number, parentDir: string | null): void => {
    if (depth > MAX_DEPTH) {
      add("depth", path, `the tree is deeper than ${MAX_DEPTH} levels`);
      return;
    }
    if (!isObject(node)) {
      add("node", path, "a node must be an object");
      return;
    }
    if (node.type === "group") {
      if (typeof node.id !== "string" || !GROUP_ID.test(node.id)) {
        add("group-id", `${path}.id`, "a group id is a g-<slug> string");
      } else if (groupIds.has(node.id)) {
        add("group-id-unique", `${path}.id`, `group id "${node.id}" is not unique`);
      } else {
        groupIds.add(node.id);
      }
      const panels = node.panels;
      if (!Array.isArray(panels) || panels.length === 0) {
        add("group-empty", `${path}.panels`, "a group has at least one panel");
        return;
      }
      panels.forEach((id, index) => claimPanel(id, `${path}.panels[${index}]`));
      if (typeof node.active !== "string" || !panels.includes(node.active)) {
        add("group-active", `${path}.active`, "active must name a panel in the group");
      }
      return;
    }
    if (node.type !== "split") {
      add("node-type", `${path}.type`, 'a node type is "split" or "group"');
      return;
    }
    if (node.dir !== "row" && node.dir !== "column") {
      add("split-dir", `${path}.dir`, 'dir is "row" or "column"');
    }
    if (parentDir !== null && node.dir === parentDir) {
      add("split-same-dir", path, "a split never has a split child of the same direction");
    }
    const children = node.children;
    const sizes = node.sizes;
    if (!Array.isArray(children) || children.length < 2) {
      add("split-children", `${path}.children`, "a split has at least two children");
      return;
    }
    if (
      !Array.isArray(sizes) ||
      sizes.length !== children.length ||
      !sizes.every((size) => typeof size === "number" && Number.isFinite(size) && size > 0)
    ) {
      add("sizes-shape", `${path}.sizes`, "sizes are one positive number per child");
    } else if (!sumsToOne(sizes as number[])) {
      add("sizes-sum", `${path}.sizes`, "sizes must sum to 1 within 1e-6");
    }
    children.forEach((child, index) =>
      walk(child, `${path}.children[${index}]`, depth + 1, typeof node.dir === "string" ? node.dir : null),
    );
  };
  walk(doc.root, "$.root", 1, null);

  if (!Array.isArray(doc.closed) || !doc.closed.every((id) => typeof id === "string")) {
    add("closed", "$.closed", "closed is a list of panel ids");
  } else {
    doc.closed.forEach((id, index) => claimPanel(id, `$.closed[${index}]`));
  }

  if (doc.maximized !== null && (typeof doc.maximized !== "string" || !seenPanels.has(doc.maximized))) {
    add("maximized", "$.maximized", "maximized is null or an open panel id");
  } else if (typeof doc.maximized === "string" && Array.isArray(doc.closed) && doc.closed.includes(doc.maximized)) {
    add("maximized", "$.maximized", "a closed panel cannot be maximized");
  }

  if (!Array.isArray(doc.collapsed) || !doc.collapsed.every((id) => typeof id === "string")) {
    add("collapsed", "$.collapsed", "collapsed is a list of group ids");
  } else {
    const seen = new Set<string>();
    doc.collapsed.forEach((id, index) => {
      if (!groupIds.has(id)) {
        add("collapsed-exists", `$.collapsed[${index}]`, `collapsed group "${id}" does not exist`);
      } else if (seen.has(id)) {
        add("collapsed-unique", `$.collapsed[${index}]`, `collapsed group "${id}" is listed twice`);
      }
      seen.add(id);
    });
    if (groupIds.size > 0 && doc.collapsed.filter((id) => groupIds.has(id)).length >= groupIds.size) {
      add("collapsed-all", "$.collapsed", "at least one group stays expanded");
    }
  }

  const shell = doc.shell;
  if (!isObject(shell)) {
    add("shell", "$.shell", "shell is an object");
  } else {
    for (const key of ["activityBar", "statusBar", "toolbar", "zen"]) {
      if (typeof shell[key] !== "boolean") {
        add("shell", `$.shell.${key}`, `${key} is a boolean`);
      }
    }
    if (shell.sideBar !== "left" && shell.sideBar !== "right") {
      add("shell", "$.shell.sideBar", 'sideBar is "left" or "right"');
    }
  }

  return errors.length === 0 ? { ok: true, layout: doc as unknown as Layout } : { ok: false, errors };
}

/** Parses stored text; the 64 KiB cap applies before parsing (a larger value is treated as corrupt). */
export function parseLayout(text: string): ValidationResult {
  if (text.length > MAX_LAYOUT_BYTES) {
    return {
      ok: false,
      errors: [{ rule: "size-limit", path: "$", message: `the stored layout is larger than ${MAX_LAYOUT_BYTES} bytes` }],
    };
  }
  let doc: unknown;
  try {
    doc = JSON.parse(text);
  } catch {
    return { ok: false, errors: [{ rule: "document", path: "$", message: "the stored layout is not JSON" }] };
  }
  return validateLayout(doc);
}

export function isLayoutNode(value: unknown): value is LayoutNode {
  return isObject(value) && (value.type === "group" || value.type === "split");
}
