// SPDX-License-Identifier: MIT

// Keybindings: chord parsing, the browser-reserved list and the resolver (`studio-workbench-v1.md` 2.3, 2.5).
//
// Chords are `Mod+Shift+P`; a sequence is space separated (`Mod+K Mod+O`). `Mod` is Ctrl on Windows/Linux
// and Cmd on macOS. Keys are `KeyboardEvent.key` names normalised to upper case. Resolution is a pure
// function over (pending chord state, event, context): match the chord, then `when`, then user beats
// default, then the most specific `when`, then the later declaration.
//
// No DOM: an "event" is any object with `key`, `code`, `ctrlKey`, `altKey`, `shiftKey`, `metaKey`.

import { evaluate, specificityOf } from "./when.js";

const KEY_ALIASES = {
  ESC: "ESCAPE",
  LEFT: "ARROWLEFT",
  RIGHT: "ARROWRIGHT",
  UP: "ARROWUP",
  DOWN: "ARROWDOWN",
  DEL: "DELETE",
  RETURN: "ENTER",
  PGUP: "PAGEUP",
  PGDN: "PAGEDOWN",
  PAGEDN: "PAGEDOWN",
};

const MODIFIERS = new Set(["MOD", "CTRL", "CONTROL", "CMD", "META", "ALT", "OPTION", "SHIFT"]);

/** Chords Chromium keeps for itself; `preventDefault` cannot capture them in a tab. */
const RESERVED = [
  ["Mod+N", "Ctrl+Alt+N"],
  ["Mod+T", null],
  ["Mod+W", "Alt+W"],
  ["Mod+Shift+N", null],
  ["Mod+Shift+T", null],
  ["Ctrl+Tab", null],
  ["Ctrl+PageUp", null],
  ["Ctrl+PageDown", null],
  ["Alt+Left", null],
  ["Alt+Right", null],
  ...[1, 2, 3, 4, 5, 6, 7, 8, 9].map((digit) => [`Mod+${digit}`, null]),
];

function normalizeKey(name) {
  const upper = name === " " ? "SPACE" : name.toUpperCase();
  return KEY_ALIASES[upper] ?? upper;
}

/**
 * Parses one step (`Mod+Shift+P`) into `{ mod, ctrl, alt, shift, meta, key }` where `mod` is abstract.
 * Returns `null` for a malformed step.
 */
export function parseStep(text) {
  const parts = text.endsWith("++") ? [...text.slice(0, -2).split("+"), "+"] : text.split("+");
  const step = { mod: false, ctrl: false, alt: false, shift: false, meta: false, key: "" };
  for (const part of parts) {
    const upper = part.toUpperCase();
    if (part === "" || (MODIFIERS.has(upper) && part !== "+")) {
      if (part === "") {
        return null;
      }
      if (upper === "MOD") step.mod = true;
      else if (upper === "CTRL" || upper === "CONTROL") step.ctrl = true;
      else if (upper === "CMD" || upper === "META") step.meta = true;
      else if (upper === "ALT" || upper === "OPTION") step.alt = true;
      else step.shift = true;
    } else if (step.key !== "") {
      return null;
    } else {
      step.key = normalizeKey(part);
    }
  }
  return step.key === "" ? null : step;
}

/** Parses a chord sequence into resolved steps for a platform, or `null` when malformed. */
export function parseChord(text, platform = "win") {
  if (typeof text !== "string" || text.trim() === "") {
    return null;
  }
  const steps = [];
  for (const piece of text.trim().split(/\s+/)) {
    const raw = parseStep(piece);
    if (raw === null) {
      return null;
    }
    const mac = platform === "mac";
    steps.push({
      ctrl: raw.ctrl || (raw.mod && !mac),
      meta: raw.meta || (raw.mod && mac),
      alt: raw.alt,
      shift: raw.shift,
      key: raw.key,
    });
  }
  return steps;
}

/** The key of a keyboard event, using the physical key when Alt rewrites the character (macOS Option). */
function eventKey(event) {
  if (event.altKey && typeof event.code === "string") {
    const letter = /^Key([A-Z])$/.exec(event.code);
    if (letter) {
      return letter[1];
    }
    const digit = /^Digit([0-9])$/.exec(event.code);
    if (digit) {
      return digit[1];
    }
  }
  return normalizeKey(String(event.key ?? ""));
}

/** One event as a resolved step. */
export function stepOfEvent(event) {
  return {
    ctrl: Boolean(event.ctrlKey),
    meta: Boolean(event.metaKey),
    alt: Boolean(event.altKey),
    shift: Boolean(event.shiftKey),
    key: eventKey(event),
  };
}

export function sameStep(a, b) {
  return a.ctrl === b.ctrl && a.meta === b.meta && a.alt === b.alt && a.shift === b.shift && a.key === b.key;
}

const KEY_LABELS = {
  ARROWLEFT: "Left",
  ARROWRIGHT: "Right",
  ARROWUP: "Up",
  ARROWDOWN: "Down",
  ESCAPE: "Esc",
  PAGEUP: "PageUp",
  PAGEDOWN: "PageDown",
  ENTER: "Enter",
  DELETE: "Delete",
  SPACE: "Space",
  TAB: "Tab",
  BACKSPACE: "Backspace",
};

function keyLabel(key) {
  return KEY_LABELS[key] ?? (key.length > 1 ? key[0] + key.slice(1).toLowerCase() : key);
}

/** The human text of a chord (`Ctrl+Shift+P`, `Ctrl+K Ctrl+O`); macOS uses `Cmd`/`Option` words. */
export function formatChord(steps, platform = "win") {
  const mac = platform === "mac";
  return steps
    .map((step) => {
      const parts = [];
      if (step.ctrl) parts.push("Ctrl");
      if (step.alt) parts.push(mac ? "Option" : "Alt");
      if (step.shift) parts.push("Shift");
      if (step.meta) parts.push(mac ? "Cmd" : "Win");
      parts.push(keyLabel(step.key));
      return parts.join("+");
    })
    .join(" ");
}

/**
 * Checks a chord string against the browser-reserved list. Returns `null` when it is usable, else
 * `{ reserved, fallback }` naming the reserved chord and, where the spec names one, its fallback.
 */
export function reservedProblem(text) {
  for (const platform of ["win", "mac"]) {
    const steps = parseChord(text, platform);
    if (steps === null) {
      continue;
    }
    for (const [reserved, fallback] of RESERVED) {
      const reservedSteps = parseChord(reserved, platform);
      if (steps.some((step) => sameStep(step, reservedSteps[0]))) {
        return { reserved, fallback };
      }
    }
  }
  return null;
}

/** Why a chord string is not acceptable as a binding, or `null`. */
export function chordProblem(text) {
  if (parseChord(text) === null) {
    return `malformed chord '${text}'`;
  }
  const reserved = reservedProblem(text);
  if (reserved !== null) {
    const hint = reserved.fallback === null ? "" : `; use ${reserved.fallback}`;
    return `'${text}' is reserved by the browser (${reserved.reserved})${hint}`;
  }
  return null;
}

/**
 * Builds the keymap from command records and user overrides.
 *
 * `overrides` is `[{ key, command, when? }]`; a `command` starting with `-` removes that command's default
 * chord `key` (VS Code's convention). Overrides on a browser-reserved chord are dropped and listed in
 * `rejected`. `isActive(id)` gates bindings on a runnable command (a reserved command binds nothing).
 *
 * @param {{ commands: Array<any>, overrides?: Array<any>, platform?: 'win'|'mac', isActive?: (id: string) => boolean }} options
 */
export function createKeymap({ commands, overrides = [], platform = "win", isActive = () => true }) {
  const bindings = [];
  const rejected = [];
  let order = 0;
  const removals = new Set();
  for (const override of overrides) {
    if (typeof override.command === "string" && override.command.startsWith("-")) {
      removals.add(`${override.command.slice(1)}\n${formatChord(parseChord(override.key, platform) ?? [], platform)}`);
    }
  }
  function add(id, key, when, source) {
    const steps = parseChord(key, platform);
    if (steps === null) {
      rejected.push({ id, key, reason: "malformed" });
      return;
    }
    if (source === "user" && reservedProblem(key) !== null) {
      rejected.push({ id, key, reason: "reserved" });
      return;
    }
    order += 1;
    bindings.push({ id, key, steps, when: when ?? null, source, specificity: specificityOf(when), order });
  }
  for (const record of commands) {
    const binding = record.keybinding;
    if (!binding || !binding.default || record.status !== "live") {
      continue;
    }
    for (const key of [binding.default, ...(binding.also ?? [])]) {
      const steps = parseChord(key, platform);
      if (steps !== null && removals.has(`${record.id}\n${formatChord(steps, platform)}`)) {
        continue;
      }
      add(record.id, key, binding.when, "default");
    }
  }
  for (const override of overrides) {
    if (typeof override.command === "string" && !override.command.startsWith("-")) {
      add(override.command, override.key, override.when, "user");
    }
  }
  const usable = bindings.filter((binding) => isActive(binding.id));

  function rank(a, b) {
    if (a.source !== b.source) return a.source === "user" ? -1 : 1;
    if (a.specificity !== b.specificity) return b.specificity - a.specificity;
    return b.order - a.order;
  }

  return {
    bindings: usable,
    rejected,
    platform,
    /**
     * Feeds one event. `pending` is the steps typed so far in a chord (`[]` at rest). Returns
     * `{ status: 'match', id, pending: [] }`, `{ status: 'pending', pending }` (waiting for the next step)
     * or `{ status: 'none', pending: [] }`.
     */
    resolve(event, pending, context) {
      const step = stepOfEvent(event);
      if (MODIFIERS.has(step.key) || step.key === "UNIDENTIFIED" || step.key === "DEAD") {
        return { status: "none", pending, ignored: true };
      }
      const attempt = (prefix) => {
        const typed = [...prefix, step];
        const live = usable.filter(
          (binding) =>
            binding.steps.length >= typed.length &&
            typed.every((entry, index) => sameStep(entry, binding.steps[index])) &&
            (binding.when === null || evaluate(binding.when, context)),
        );
        const exact = live.filter((binding) => binding.steps.length === typed.length).sort(rank);
        if (exact.length > 0) {
          return { status: "match", id: exact[0].id, pending: [] };
        }
        if (live.length > 0) {
          return { status: "pending", pending: typed };
        }
        return null;
      };
      return attempt(pending) ?? (pending.length > 0 ? attempt([]) : null) ?? { status: "none", pending: [] };
    },
    /** The display chord of a command, user binding first; `''` when unbound. */
    chordFor(id) {
      const mine = usable.filter((binding) => binding.id === id).sort((a, b) => (a.source === b.source ? a.order - b.order : a.source === "user" ? -1 : 1));
      return mine.length === 0 ? "" : formatChord(mine[0].steps, platform);
    },
    /** Chords bound to more than one command whose `when` clauses could overlap (a report, not an error). */
    conflicts() {
      const byChord = new Map();
      for (const binding of usable) {
        const text = formatChord(binding.steps, platform);
        const list = byChord.get(text) ?? [];
        list.push(binding);
        byChord.set(text, list);
      }
      return [...byChord.entries()]
        .filter(([, list]) => new Set(list.map((binding) => binding.id)).size > 1)
        .map(([chord, list]) => ({ chord, commands: list.map((binding) => ({ id: binding.id, when: binding.when })) }));
    },
  };
}
