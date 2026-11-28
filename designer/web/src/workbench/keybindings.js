// SPDX-License-Identifier: MIT

// User keybinding overrides (`studio-workbench-v1.md` section 2.3, slice W8 #565): the document that is stored,
// exported and imported, its validation, the edits the settings UI makes to it, the conflict report, and the
// table the keyboard-shortcuts pane lists.
//
// Pure, like the rest of the core: no DOM, no React. Storage is handed in (`localStorage` or a stand-in), and every
// access to it is wrapped, because reading `localStorage` throws in a blocked profile and a preference is never a
// page error. An import is all-or-nothing: a document with one bad entry is refused whole, with the reasons,
// and the stored overrides are untouched.

import { clauseError, clauseKeys } from "./when.js";
import { isKnownContextKey } from "./context.js";
import { chordProblem, createKeymap, formatChord, parseChord, stepOfEvent } from "./keys.js";

export const KEYBINDINGS_STORAGE_KEY = "tessera.studio.keybindings";
export const KEYBINDINGS_FORMAT = "tessera-studio-keybindings";
export const KEYBINDINGS_VERSION = 1;
export const MAX_OVERRIDES = 500;
export const MAX_KEYBINDINGS_BYTES = 64 * 1024;

/** Keys that are a modifier or not a key at all: a recorder waits for the next event instead of binding them. */
const NOT_A_KEY = new Set(["CONTROL", "SHIFT", "ALT", "ALTGRAPH", "META", "OS", "UNIDENTIFIED", "DEAD"]);

const isObject = (value) => typeof value === "object" && value !== null && !Array.isArray(value);

/**
 * Checks a parsed or textual overrides document against the command records.
 *
 * Accepted shapes: an array of `{ key, command, when? }`, or `{ format?, version, bindings: [...] }`. A command
 * that starts with `-` removes that command's default chord `key`. Returns `{ ok: true, overrides }` or
 * `{ ok: false, errors }` where every error names the entry; nothing is partially accepted.
 *
 * @param {unknown} input text or a parsed document
 * @param {readonly any[]} commands the command records
 */
export function validateOverrides(input, commands) {
  let doc = input;
  if (typeof input === "string") {
    if (input.length > MAX_KEYBINDINGS_BYTES) {
      return { ok: false, errors: [`the document is larger than ${MAX_KEYBINDINGS_BYTES} bytes`] };
    }
    try {
      doc = JSON.parse(input);
    } catch {
      return { ok: false, errors: ["the text is not JSON"] };
    }
  }
  let list = doc;
  if (isObject(doc)) {
    if (doc.format !== undefined && doc.format !== KEYBINDINGS_FORMAT) {
      return { ok: false, errors: [`format must be "${KEYBINDINGS_FORMAT}"`] };
    }
    if (typeof doc.version === "number" && doc.version > KEYBINDINGS_VERSION) {
      return { ok: false, errors: [`keybindings version ${doc.version} is from a newer studio; this one reads up to ${KEYBINDINGS_VERSION}`] };
    }
    if (doc.version !== KEYBINDINGS_VERSION) {
      return { ok: false, errors: [`version must be ${KEYBINDINGS_VERSION}`] };
    }
    list = doc.bindings;
  }
  if (!Array.isArray(list)) {
    return { ok: false, errors: ["expected an array of bindings or { version, bindings }"] };
  }
  if (list.length > MAX_OVERRIDES) {
    return { ok: false, errors: [`${list.length} bindings; the limit is ${MAX_OVERRIDES}`] };
  }
  const byId = new Map(commands.map((record) => [record.id, record]));
  const errors = [];
  const overrides = [];
  const seen = new Set();
  list.forEach((entry, index) => {
    const at = `bindings[${index}]`;
    if (!isObject(entry) || typeof entry.key !== "string" || typeof entry.command !== "string") {
      errors.push(`${at}: expected { key: string, command: string }`);
      return;
    }
    const removal = entry.command.startsWith("-");
    const id = removal ? entry.command.slice(1) : entry.command;
    const record = byId.get(id);
    if (record === undefined) {
      errors.push(`${at}: unknown command '${id}'`);
      return;
    }
    if (record.status === "removed") {
      errors.push(`${at}: command '${id}' was retired${typeof record.replacedBy === "string" ? `; use '${record.replacedBy}'` : ""}`);
      return;
    }
    const problem = removal ? (parseChord(entry.key) === null ? `malformed chord '${entry.key}'` : null) : chordProblem(entry.key);
    if (problem !== null) {
      errors.push(`${at}: ${problem}`);
      return;
    }
    let when;
    if (entry.when !== undefined && entry.when !== null) {
      const bad = typeof entry.when !== "string" ? "when must be a string" : clauseError(entry.when);
      if (bad !== null) {
        errors.push(`${at}: when '${String(entry.when)}': ${bad}`);
        return;
      }
      const unknown = clauseKeys(entry.when).find((key) => !isKnownContextKey(key));
      if (unknown !== undefined) {
        errors.push(`${at}: when names unknown context key '${unknown}'`);
        return;
      }
      when = entry.when;
    }
    const identity = `${entry.command}\n${entry.key}\n${when ?? ""}`;
    if (!seen.has(identity)) {
      seen.add(identity);
      overrides.push(when === undefined ? { key: entry.key, command: entry.command } : { key: entry.key, command: entry.command, when });
    }
  });
  return errors.length === 0 ? { ok: true, overrides } : { ok: false, errors };
}

/** The exported text: a versioned document, two-space indented so it diffs and hand-edits well. */
export function serializeOverrides(overrides) {
  return `${JSON.stringify({ format: KEYBINDINGS_FORMAT, version: KEYBINDINGS_VERSION, bindings: overrides }, null, 2)}\n`;
}

const isDefaultChord = (record) => [record.keybinding?.default, ...(record.keybinding?.also ?? [])].filter((key) => typeof key === "string");

/** Rebinds a command: its other overrides go, its defaults are removed, and `key` becomes its one user chord. */
export function setBinding(overrides, record, key, when) {
  const kept = overrides.filter((entry) => entry.command !== record.id && entry.command !== `-${record.id}`);
  const removals = isDefaultChord(record).map((chord) => ({ key: chord, command: `-${record.id}` }));
  return [...kept, ...removals, when === undefined ? { key, command: record.id } : { key, command: record.id, when }];
}

/** Leaves a command with no chord at all: every default is removed and no user chord remains. */
export function clearBinding(overrides, record) {
  const kept = overrides.filter((entry) => entry.command !== record.id && entry.command !== `-${record.id}`);
  return [...kept, ...isDefaultChord(record).map((chord) => ({ key: chord, command: `-${record.id}` }))];
}

/** Puts a command back to its default chord by dropping every override that names it. */
export function resetBinding(overrides, id) {
  return overrides.filter((entry) => entry.command !== id && entry.command !== `-${id}`);
}

/** The text of one event as a chord a person could have typed (`Mod+Shift+P`), or `null` for a bare modifier. */
export function chordOfEvent(event, platform = "win") {
  const step = stepOfEvent(event);
  if (NOT_A_KEY.has(step.key)) {
    return null;
  }
  const mac = platform === "mac";
  const parts = [];
  if (mac ? step.meta : step.ctrl) parts.push("Mod");
  if (mac && step.ctrl) parts.push("Ctrl");
  if (!mac && step.meta) parts.push("Meta");
  if (step.alt) parts.push("Alt");
  if (step.shift) parts.push("Shift");
  parts.push(step.key);
  return parts.join("+");
}

/**
 * The other commands that a chord would collide with if `id` took it: every command bound to the same chord
 * text, whatever its `when` (a report for the person to judge, not a refusal).
 */
export function collisionsFor({ commands, overrides, platform = "win", isActive = () => true }, id, key) {
  const steps = parseChord(key, platform);
  if (steps === null) {
    return [];
  }
  const text = formatChord(steps, platform);
  const keymap = createKeymap({ commands, overrides, platform, isActive });
  return [...new Set(keymap.bindings.filter((binding) => binding.id !== id && formatChord(binding.steps, platform) === text).map((binding) => binding.id))];
}

/**
 * The rows of the keyboard-shortcuts pane: every live command with a handler, its current chord, where the chord
 * comes from, its default, the commands whose scope overlaps on the chord (`conflicts`) and those scoped differently (`shared`).
 */
export function keybindingRows({ commands, overrides, platform = "win", isActive = () => true }) {
  const keymap = createKeymap({ commands, overrides, platform, isActive });
  // Two commands on one chord collide only when their scopes can both be true: an unscoped binding overlaps
  // everything, and two bindings with the same `when` overlap. Different `when` clauses (F2 in the tree and
  // F2 in the editor) are listed as `shared`, which is information, not a problem.
  const conflicting = new Map();
  const shared = new Map();
  const add = (map, id, other) => map.set(id, [...new Set([...(map.get(id) ?? []), other])]);
  for (const conflict of keymap.conflicts()) {
    for (const entry of conflict.commands) {
      for (const other of conflict.commands) {
        if (other.id === entry.id) continue;
        const overlap = entry.when === null || other.when === null || entry.when === other.when;
        add(overlap ? conflicting : shared, entry.id, other.id);
      }
    }
  }
  const rows = [];
  for (const record of commands) {
    if (record.status !== "live" || !isActive(record.id)) {
      continue;
    }
    const mine = keymap.bindings.filter((binding) => binding.id === record.id);
    const user = mine.find((binding) => binding.source === "user");
    const defaults = isDefaultChord(record);
    const defaultText = defaults.length === 0 ? "" : formatChord(parseChord(defaults[0], platform) ?? [], platform);
    const source = user !== undefined ? "user" : mine.length > 0 ? "default" : defaults.length > 0 ? "removed" : "unbound";
    rows.push({
      id: record.id,
      title: record.title,
      category: record.category,
      chord: keymap.chordFor(record.id),
      source,
      defaultChord: defaultText,
      conflicts: conflicting.get(record.id) ?? [],
      shared: shared.get(record.id) ?? [],
    });
  }
  return rows.sort((a, b) => a.category.localeCompare(b.category) || a.title.localeCompare(b.title));
}

/**
 * Reads the stored overrides. A missing, unreadable, corrupt or invalid value gives no overrides (the defaults) and, for
 * the last two, a notice; the stored text is left alone so a newer studio's document is not destroyed.
 * @returns {{ overrides: any[], notice: string | null }}
 */
export function loadOverrides(storage, commands) {
  let raw = null;
  try {
    raw = storage === null || storage === undefined ? null : storage.getItem(KEYBINDINGS_STORAGE_KEY);
  } catch {
    return { overrides: [], notice: null };
  }
  if (raw === null) {
    return { overrides: [], notice: null };
  }
  const checked = validateOverrides(raw, commands);
  return checked.ok ? { overrides: checked.overrides, notice: null } : { overrides: [], notice: "The saved keybindings could not be read; using the defaults" };
}

/** Writes the overrides (an empty list clears the key). `false` when storage is missing or refused the write. */
export function saveOverrides(storage, overrides) {
  if (storage === null || storage === undefined) {
    return false;
  }
  try {
    if (overrides.length === 0) {
      storage.removeItem?.(KEYBINDINGS_STORAGE_KEY);
    } else {
      storage.setItem(KEYBINDINGS_STORAGE_KEY, serializeOverrides(overrides));
    }
    return true;
  } catch {
    return false;
  }
}
