// SPDX-License-Identifier: MIT

// The menu model (`studio-workbench-v1.md` section 3): `menus.json` is data, this module turns it plus the
// registry's live state into the tree a renderer draws. It carries no behaviour; an item names a command id
// and literal arguments, and activating it is `registry.run`.
//
// Item grammar in `menus.json`:
//   { command, args?, title?, group? }            a command (a radio or check item is derived, below)
//   { submenu, title, mnemonic?, group?, items }  a nested menu (`submenu` is its stable id)
//   { dynamic, command, group? }                  entries a page-registered provider returns at open time
//                                                 (a context menu's provider also gets the clicked target)
// `group` is `name@order`; a separator is drawn between groups, never authored. A reserved command's item is
// hidden until the command is live. A command with a `checked` clause is a toggle (`check`) or, when the item
// carries arguments, one entry of a radio group (`radio`).

function parseGroup(group) {
  const at = typeof group === "string" ? group.indexOf("@") : -1;
  return at < 0 ? { name: group ?? "", order: 0 } : { name: group.slice(0, at), order: Number(group.slice(at + 1)) || 0 };
}

/** Walks every item of the document (menus, submenus, contexts, toolbar), calling `visit(item, where)`. */
export function walkMenus(doc, visit) {
  const walk = (items, where) => {
    for (const item of items) {
      visit(item, where);
      if (item.items) {
        walk(item.items, `${where} > ${item.title ?? item.submenu}`);
      }
    }
  };
  for (const menu of doc.menus ?? []) {
    walk(menu.items, `menu ${menu.id}`);
  }
  for (const [name, items] of Object.entries(doc.contexts ?? {})) {
    walk(items, `context ${name}`);
  }
  for (const item of doc.toolbar ?? []) {
    visit(item, "toolbar");
  }
  for (const item of doc.statusBar ?? []) {
    visit(item, "statusBar");
  }
}

/**
 * Problems in a menus document against the command records: unknown command ids, bad groups, a top-level
 * mnemonic that is not a letter of its title or is used twice, missing titles on submenus and dynamics.
 * @returns {string[]}
 */
export function validateMenus(doc, records) {
  const problems = [];
  const ids = new Set(records.map((record) => record.id));
  walkMenus(doc, (item, where) => {
    if (item.command !== undefined && !ids.has(item.command)) {
      problems.push(`${where}: unknown command '${item.command}'`);
    }
    if (item.submenu !== undefined && (typeof item.title !== "string" || !Array.isArray(item.items))) {
      problems.push(`${where}: submenu '${item.submenu}' needs a title and items`);
    }
    if (item.dynamic !== undefined && item.command === undefined) {
      problems.push(`${where}: dynamic '${item.dynamic}' needs a command`);
    }
    if (item.command === undefined && item.submenu === undefined && item.dynamic === undefined && item.widget === undefined) {
      problems.push(`${where}: item names no command, submenu or dynamic`);
    }
    if (item.group !== undefined && !/^[a-z]+@-?\d+$/.test(item.group)) {
      problems.push(`${where}: bad group '${item.group}' (expected name@order)`);
    }
  });
  const seen = new Set();
  for (const menu of doc.menus ?? []) {
    const mnemonic = menu.mnemonic;
    if (typeof mnemonic !== "string" || !menu.title.toUpperCase().includes(mnemonic.toUpperCase())) {
      problems.push(`menu ${menu.id}: mnemonic '${mnemonic}' is not a letter of '${menu.title}'`);
    } else if (seen.has(mnemonic.toUpperCase())) {
      problems.push(`menu ${menu.id}: mnemonic '${mnemonic}' is used twice`);
    }
    seen.add(String(mnemonic).toUpperCase());
  }
  return problems;
}

function sortGroups(items) {
  const firstSeen = new Map();
  items.forEach((item, index) => {
    const { name } = parseGroup(item.group);
    if (!firstSeen.has(name)) {
      firstSeen.set(name, index);
    }
  });
  return items
    .map((item, index) => ({ item, index, group: parseGroup(item.group) }))
    .sort((a, b) => firstSeen.get(a.group.name) - firstSeen.get(b.group.name) || a.group.order - b.group.order || a.index - b.index);
}

/**
 * Builds the visible tree for a list of menu items.
 * @param {Array<any>} items the authored items
 * @param {{ registry: any, providers?: Record<string, () => Array<{label: string, args?: object, detail?: string}>>, keymap?: any, keyPrefix: string }} deps
 */
export function buildItems(items, deps) {
  const { registry, providers = {}, keyPrefix } = deps;
  const keymap = deps.keymap ?? registry.keymap();
  const out = [];
  let lastGroup = null;
  const push = (entry, group) => {
    if (out.length > 0 && lastGroup !== null && group !== lastGroup) {
      out.push({ kind: "separator", key: `${keyPrefix}/sep${out.length}` });
    }
    lastGroup = group;
    out.push(entry);
  };
  const forCommand = (command, args, title, group, key, detail, provided) => {
    const state = registry.describe(command, args);
    if (state === null || !state.visible) {
      return;
    }
    // A provider may state a per-entry check (the Panels submenu: one command, a check per panel); the
    // command's own clause cannot index by argument.
    const checked = provided === undefined ? state.checked : provided;
    const toggle = checked !== null;
    push(
      {
        kind: toggle ? (args === undefined || provided !== undefined ? "check" : "radio") : "item",
        key,
        label: title ?? state.title,
        detail,
        command,
        args,
        chord: args === undefined ? keymap.chordFor(command) : "",
        enabled: state.enabled,
        reason: state.reason,
        checked: checked === true,
        description: state.record.description ?? "",
      },
      group,
    );
  };
  sortGroups(items).forEach(({ item, group }, position) => {
    const key = `${keyPrefix}/${item.submenu ?? item.command ?? item.dynamic}${position}`;
    if (item.items !== undefined) {
      const children = buildItems(item.items, { ...deps, keyPrefix: key });
      if (children.length === 0 && item.showEmpty !== true) {
        return;
      }
      push(
        { kind: "submenu", key, label: item.title, mnemonic: item.mnemonic, items: children, enabled: children.some((child) => child.kind !== "separator" && child.enabled !== false), reason: "", description: "" },
        group.name,
      );
    } else if (item.dynamic !== undefined) {
      const provide = providers[item.dynamic];
      const entries = provide === undefined ? [] : provide(deps.target);
      entries.forEach((entry, index) => forCommand(item.command, entry.args, entry.label, group.name, `${key}/${index}`, entry.detail, entry.checked));
    } else {
      forCommand(item.command, item.args, item.title, group.name, key);
    }
  });
  while (out.length > 0 && out[out.length - 1].kind === "separator") {
    out.pop();
  }
  return out;
}

/**
 * The menu bar: each top-level menu with its built items. A menu with nothing visible is left out (`Go`
 * appears when its first command goes live).
 */
export function buildMenuBar(doc, deps) {
  const keymap = deps.keymap ?? deps.registry.keymap();
  return (doc.menus ?? [])
    .map((menu) => ({ id: menu.id, title: menu.title, mnemonic: menu.mnemonic, items: buildItems(menu.items, { ...deps, keymap, keyPrefix: menu.id }) }))
    .filter((menu) => menu.items.some((item) => item.kind !== "separator"));
}

/** A context menu (`tree.item`, `editor`, `panel.tab`, `canvas`) as built items. */
export function buildContextMenu(doc, name, deps, target) {
  return buildItems(doc.contexts?.[name] ?? [], { ...deps, target, keymap: deps.keymap ?? deps.registry.keymap(), keyPrefix: `ctx.${name}` });
}

/**
 * The toolbar's quick actions as built entries (only live commands; emphasis carried through). `section` is
 * `"statusBar"` for the status bar's items, which use the same shape: a command, or a widget the page draws.
 */
export function buildToolbar(doc, deps, section = "toolbar") {
  const keymap = deps.keymap ?? deps.registry.keymap();
  const out = [];
  for (const entry of doc[section] ?? []) {
    if (entry.widget !== undefined) {
      out.push({ key: `${section}/${entry.widget}`, widget: entry.widget });
      continue;
    }
    const state = deps.registry.describe(entry.command, entry.args);
    if (state === null || !state.visible) {
      continue;
    }
    out.push({
      key: `${section}/${entry.command}`,
      command: entry.command,
      args: entry.args,
      label: entry.title ?? state.record.titleShort ?? state.record.title,
      icon: entry.icon,
      emphasis: entry.emphasis ?? "normal",
      enabled: state.enabled,
      reason: state.reason,
      checked: state.checked,
      chord: keymap.chordFor(entry.command),
      description: state.record.description ?? "",
    });
  }
  return out;
}
