// SPDX-License-Identifier: MIT

// The settings surface's page-side tools (slice W8 #565): the keybinding overrides the keyboard-shortcuts pane edits and
// the layout import, export and saved presets the Layout pane drives. The rules are the pure modules in `workbench/`
// (`keybindings.js`, `layout-io.ts`); this file only joins them to the registry, `localStorage` and the dock store.
//
// Nothing here throws on a bad input or a blocked store: a refusal is a value with a sentence, and a refused
// import leaves the overrides and the layout exactly as they were.

import {
  clearBinding,
  chordOfEvent,
  collisionsFor,
  keybindingRows,
  loadOverrides,
  resetBinding,
  saveOverrides,
  serializeOverrides,
  setBinding,
  validateOverrides,
  type KeyEventLike,
  type KeybindingOverride,
  type KeybindingRow,
  type NotificationInput,
  type Registry,
} from "./workbench/index.js";
import {
  browserStorage,
  deleteUserPreset,
  exportLayout,
  importLayout,
  listUserPresets,
  saveUserPreset,
  type Layout,
  type UserPreset,
} from "./workbench/layout";
import type { Workbench as Dock } from "./dock/use-workbench";
import type { LayoutUndo } from "./page-layout-commands";

export type SettingsTab = "keybindings" | "layout" | "appearance";

export type Outcome = { ok: true; message: string } | { ok: false; message: string };

export interface KeybindingTools {
  rows(): KeybindingRow[];
  overrides(): readonly KeybindingOverride[];
  /** Rebinds a command to the chord `text` (`Mod+Shift+L`, or a sequence); the other commands now on it are named. */
  rebind(id: string, text: string): Outcome & { collisions?: string[] };
  /** Leaves a command with no chord. */
  clear(id: string): Outcome;
  /** Back to the default chord. */
  reset(id: string): Outcome;
  resetAll(): Outcome;
  exportText(): string;
  importText(text: string): Outcome;
  /** The recorder: the chord text of a key event, or `null` for a bare modifier. */
  record(event: KeyEventLike): string | null;
  title(id: string): string;
}

export interface LayoutTools {
  exportText(): string;
  importText(text: string): Outcome;
  presets(): UserPreset[];
  savePreset(name: string): Outcome;
  applyPreset(name: string): Outcome;
  deletePreset(name: string): void;
}

export interface SettingsTools {
  keybindings: KeybindingTools;
  layout: LayoutTools;
}

export interface SettingsHost {
  registry: Registry;
  dock: () => Dock;
  notify: (input: NotificationInput) => number | null;
  undo: { current: LayoutUndo | null };
}

const WHY_NOT_SAVED = " It applies to this session only: the browser would not store it.";

/** Creates the tools and applies the stored overrides to the registry (a corrupt store is a warning, not an error). */
export function createSettingsTools(host: SettingsHost): SettingsTools {
  const { registry } = host;
  const storage = browserStorage();
  const loaded = loadOverrides(storage, registry.records);
  let overrides: KeybindingOverride[] = loaded.overrides;
  registry.setOverrides(overrides);
  if (loaded.notice !== null) {
    host.notify({ message: loaded.notice, severity: "warning", key: "keybindings-notice" });
  }
  const record = (id: string) => registry.records.find((entry) => entry.id === id);
  const inputs = () => {
    const active = new Set(registry.handlerIds());
    return { commands: registry.records, overrides, platform: registry.platform, isActive: (id: string) => active.has(id) };
  };
  const apply = (next: KeybindingOverride[], done: string): Outcome => {
    overrides = next;
    registry.setOverrides(next);
    return { ok: true, message: saveOverrides(storage, next) ? done : done + WHY_NOT_SAVED };
  };
  const titleOf = (id: string) => record(id)?.title ?? id;

  const keybindings: KeybindingTools = {
    rows: () => keybindingRows(inputs()),
    overrides: () => overrides,
    title: titleOf,
    record: (event) => chordOfEvent(event, registry.platform),
    rebind(id, text) {
      const target = record(id);
      if (target === undefined) {
        return { ok: false, message: `There is no command '${id}'.` };
      }
      const key = text.trim();
      const checked = validateOverrides([{ key, command: id }], registry.records);
      if (!checked.ok) {
        return { ok: false, message: checked.errors.join(" ").replace(/^bindings\[0\]: /, "") };
      }
      const collisions = collisionsFor(inputs(), id, key);
      const outcome = apply(setBinding(overrides, target, key), `${target.title} is now ${key}.`);
      return { ...outcome, collisions };
    },
    clear: (id) => {
      const target = record(id);
      return target === undefined ? { ok: false, message: `There is no command '${id}'.` } : apply(clearBinding(overrides, target), `${target.title} has no shortcut.`);
    },
    reset: (id) => apply(resetBinding(overrides, id), `${titleOf(id)} is back to its default.`),
    resetAll: () => apply([], "Every shortcut is back to its default."),
    exportText: () => serializeOverrides(overrides),
    importText(text) {
      const checked = validateOverrides(text, registry.records);
      if (!checked.ok) {
        const shown = checked.errors.slice(0, 5).join(" ");
        const more = checked.errors.length > 5 ? ` (${checked.errors.length - 5} more)` : "";
        return { ok: false, message: `Nothing was imported: ${shown}${more}` };
      }
      return apply(checked.overrides, `Imported ${checked.overrides.length} shortcut ${checked.overrides.length === 1 ? "entry" : "entries"}.`);
    },
  };

  const layout: LayoutTools = {
    exportText: () => exportLayout(host.dock().store.get()),
    importText: (text) => {
      const imported = importLayout(text);
      return imported.ok ? replaceLayout(host, imported.layout, "Layout imported") : { ok: false, message: `Nothing was imported: ${imported.message}` };
    },
    presets: () => listUserPresets(storage),
    savePreset(name) {
      const saved = saveUserPreset(storage, name, host.dock().store.get());
      return saved.ok ? { ok: true, message: `Saved the layout as '${name.trim()}'.` } : { ok: false, message: saved.message };
    },
    applyPreset(name) {
      const found = listUserPresets(storage).find((preset) => preset.name === name);
      return found === undefined ? { ok: false, message: `There is no saved layout '${name}'.` } : replaceLayout(host, found.layout, `Layout: ${name}`);
    },
    deletePreset: (name) => void deleteUserPreset(storage, name),
  };
  return { keybindings, layout };
}

/** Puts a whole layout in place with one undo step, like a built-in preset. */
function replaceLayout(host: SettingsHost, next: Layout, message: string): Outcome {
  const dock = host.dock();
  const before = dock.store.get();
  dock.store.restore(next);
  host.undo.current = { before, after: next };
  host.notify({ message, key: "layout-change", actions: [{ label: "Undo layout change", command: "view.undoLayoutChange" }] });
  return { ok: true, message };
}
