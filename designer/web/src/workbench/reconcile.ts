// SPDX-License-Identifier: MIT

// `reconcile` (studio-workbench-layout-v1.md section 2.3): makes a migrated, validated document match the
// running build. It is not a migration: shipping a new panel does not bump the version.

import { defaultContext } from "./layout-context";
import { defaultTarget, finish, insertPanel } from "./place";
import { cloneLayout, groupsOf, normalize, openPanels } from "./tree";
import type { Context, Layout } from "./types";

export function reconcile(input: Layout, ctx: Context = defaultContext): Layout {
  const layout = cloneLayout(input);
  const known = new Set(ctx.defs.filter((def) => def.reserved !== true).map((def) => def.id));

  // 1 and 2: panels without a definition, and reserved ones, are dropped (emptied groups go with them).
  for (const group of groupsOf(layout.root)) {
    group.panels = group.panels.filter((id) => known.has(id));
    if (group.panels.length > 0 && !group.panels.includes(group.active)) {
      group.active = group.panels[0];
    }
  }
  layout.closed = layout.closed.filter((id) => known.has(id));
  const root = normalize(layout.root);
  if (root === null) {
    return reconcile(cloneLayout(ctx.defaults), ctx);
  }
  layout.root = root;
  finish(layout);

  // 3: a panel that is defined, not in any group and not closed is new; it takes its default place.
  const open = new Set(openPanels(layout));
  for (const def of ctx.defs) {
    if (!known.has(def.id) || open.has(def.id) || layout.closed.includes(def.id)) {
      continue;
    }
    insertPanel(layout, def.id, defaultTarget(layout, def.id, ctx));
    open.add(def.id);
  }

  // 4: a panel that cannot be closed (the editor) is restored if it is missing.
  for (const def of ctx.defs) {
    if (def.closable === false && known.has(def.id) && !open.has(def.id)) {
      layout.closed = layout.closed.filter((id) => id !== def.id);
      insertPanel(layout, def.id, defaultTarget(layout, def.id, ctx));
      open.add(def.id);
    }
  }

  // The dock always keeps one expanded group.
  if (layout.collapsed.length >= groupsOf(layout.root).length) {
    layout.collapsed = [];
  }
  layout.version = Math.max(layout.version, ctx.defaults.version);
  return finish(layout);
}
