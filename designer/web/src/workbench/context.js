// SPDX-License-Identifier: MIT

// The context keys the page publishes (`studio-workbench-v1.md` section 2.2) and the store that holds them.
//
// Keys are a closed vocabulary: a `commands.json` clause that names a key nobody publishes is a typo, and a
// test (`preview_workbench.mjs`) fails it instead of letting the command sit disabled forever. Patterns
// cover the keys that are parameterised (`panel.<id>.open`, `service.<name>.ready`, `arg.<name>`).
//
// Additions to the spec's table are marked and recorded in the spec: the device and run keys exist so a
// radio or checkbox menu item can derive its `checked` state (`device.viewport == arg.value`), and
// `tree.focus` scopes F2 and Delete to the project tree.

/** The fixed keys. */
export const CONTEXT_KEYS = Object.freeze([
  "project.open",
  "project.source",
  "project.busy",
  "project.dirtyCount",
  "project.hasRecent",
  "project.folderSupported",
  "editor.focus",
  "editor.hasSelection",
  "canvas.available",
  "canvas.running",
  "layout.phone",
  "layout.isDefault",
  "device.foldable",
  "focus.palette",
  "focus.menu",
  // Additions (W1): the page's own state a toggle or radio reads, and the tree's focus.
  "tree.focus",
  "project.multiComponent",
  "device.profile",
  "device.skin",
  "device.viewport",
  "device.dpr",
  "device.fold",
  "device.keepState",
  "ui.theme",
  "run.picker",
  "run.inspector",
  "run.enabled",
  "run.component",
  "notify.dnd",
  "notify.unread",
  "notify.any",
]);

const PATTERNS = [/^panel\.[A-Za-z0-9_-]+\.(open|visible)$/, /^service\.[A-Za-z0-9_-]+\.ready$/, /^arg\.[A-Za-z0-9_]+$/];

const fixed = new Set(CONTEXT_KEYS);

/** True when `key` is a published context key. */
export function isKnownContextKey(key) {
  return fixed.has(key) || PATTERNS.some((pattern) => pattern.test(key));
}

/**
 * A flat record of context keys with change notification. `set` replaces values and notifies once, and only
 * when something actually changed, so a re-publish of the same state wakes no menu.
 */
export function createContext(initial = {}) {
  let values = { ...initial };
  const listeners = new Set();
  return {
    snapshot: () => values,
    get: (key) => (Object.hasOwn(values, key) ? values[key] : false),
    set(patch) {
      let changed = false;
      const next = { ...values };
      for (const [key, value] of Object.entries(patch)) {
        if (next[key] !== value) {
          next[key] = value;
          changed = true;
        }
      }
      if (changed) {
        values = next;
        for (const listener of [...listeners]) {
          listener(values);
        }
      }
      return changed;
    },
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}
