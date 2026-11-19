// SPDX-License-Identifier: MIT

// The command palette, quick open and go-to-line logic (issue #558; `studio-workbench-v1.md` 2.1, 3.1, 6).
// Pure functions over the registry's records and a file list; the overlay that draws them is React.
//
// One input serves three modes, chosen by its prefix as in VS Code: `>` searches commands, `:` goes to a
// line, anything else searches files (and `file:12` opens the file at line 12). The palette opens with `>`,
// Quick Open with an empty input, Go to Line with `:`.
//
// Ranking (the layout spec defers it here): a fuzzy subsequence score (fzy's weights: consecutive matches,
// word starts, slashes, camel humps, small gap penalties); for a file the basename is scored too and wins
// when it is the better match; ties break by how recently the item was used, then by shorter text, then
// alphabetically. With an empty query the recent items come first, most recent first, then the rest in
// alphabetical order.

const SCORE_MATCH_CONSECUTIVE = 1.0;
const SCORE_GAP_LEADING = -0.005;
const SCORE_GAP_TRAILING = -0.005;
const SCORE_GAP_INNER = -0.01;
const BONUS_SLASH = 0.9;
const BONUS_WORD = 0.8;
const BONUS_CAPITAL = 0.7;
const BONUS_DOT = 0.6;

function bonusAt(text, index) {
  if (index === 0) {
    return BONUS_SLASH;
  }
  const previous = text[index - 1];
  const here = text[index];
  if (previous === "/" || previous === "\\") return BONUS_SLASH;
  if (previous === " " || previous === "_" || previous === "-" || previous === ":") return BONUS_WORD;
  if (previous === ".") return BONUS_DOT;
  if (previous === previous.toLowerCase() && previous !== previous.toUpperCase() && here !== here.toLowerCase()) return BONUS_CAPITAL;
  return 0;
}

/**
 * A fuzzy subsequence match of `query` in `text`: `{ score, positions }` (the matched character indices) or
 * `null` when `query` is not a subsequence. Case-insensitive; an empty query matches everything with 0.
 */
export function fuzzyMatch(query, text) {
  const n = query.length;
  const m = text.length;
  if (n === 0) {
    return { score: 0, positions: [] };
  }
  if (n > m) {
    return null;
  }
  const q = query.toLowerCase();
  const t = text.toLowerCase();
  // Cheap subsequence rejection before the quadratic table.
  let at = 0;
  for (let i = 0; i < m && at < n; i += 1) {
    if (t[i] === q[at]) at += 1;
  }
  if (at < n) {
    return null;
  }
  const bonus = Array.from({ length: m }, (_, j) => bonusAt(text, j));
  const D = Array.from({ length: n }, () => new Float64Array(m).fill(-Infinity));
  const M = Array.from({ length: n }, () => new Float64Array(m).fill(-Infinity));
  for (let i = 0; i < n; i += 1) {
    let prevBest = -Infinity;
    const gap = i === n - 1 ? SCORE_GAP_TRAILING : SCORE_GAP_INNER;
    for (let j = 0; j < m; j += 1) {
      if (q[i] === t[j]) {
        let score = -Infinity;
        if (i === 0) {
          score = j * SCORE_GAP_LEADING + bonus[j];
        } else if (j > 0) {
          score = Math.max(M[i - 1][j - 1] + bonus[j], D[i - 1][j - 1] + SCORE_MATCH_CONSECUTIVE);
        }
        D[i][j] = score;
        prevBest = Math.max(score, prevBest + gap);
      } else {
        prevBest += gap;
      }
      M[i][j] = prevBest;
    }
  }
  const final = M[n - 1][m - 1];
  if (final === -Infinity) {
    return null;
  }
  const positions = new Array(n);
  let matchRequired = false;
  for (let i = n - 1, j = m - 1; i >= 0; i -= 1) {
    for (; j >= 0; j -= 1) {
      if (D[i][j] !== -Infinity && (matchRequired || D[i][j] === M[i][j])) {
        matchRequired = i > 0 && j > 0 && M[i][j] === D[i - 1][j - 1] + SCORE_MATCH_CONSECUTIVE;
        positions[i] = j;
        j -= 1;
        break;
      }
    }
  }
  return { score: final, positions };
}

/** A recently-used list: most recent first, unique, capped. JSON-serialisable through `toJSON`/`load`. */
export function createRecentList(limit = 8) {
  let items = [];
  return {
    touch(id) {
      items = [id, ...items.filter((entry) => entry !== id)].slice(0, limit);
    },
    list: () => [...items],
    indexOf: (id) => items.indexOf(id),
    load(saved) {
      items = Array.isArray(saved) ? [...new Set(saved.filter((entry) => typeof entry === "string"))].slice(0, limit) : [];
    },
    toJSON: () => [...items],
  };
}

/**
 * Ranks items for a query. Items are `{ id, label, detail?, basename? }`; a file passes its `basename` so it
 * can win on the file name alone. Returns `[{ item, score, positions, basenameMatch }]` best first.
 * @param {string} query
 * @param {Array<{id: string, label: string, detail?: string, basename?: string}>} items
 * @param {{ recent?: { indexOf: (id: string) => number }, limit?: number }} [options]
 */
export function rankItems(query, items, options = {}) {
  const { recent, limit = 200 } = options;
  const recency = (id) => {
    const index = recent === undefined ? -1 : recent.indexOf(id);
    return index < 0 ? Infinity : index;
  };
  const scored = [];
  for (const item of items) {
    if (query === "") {
      scored.push({ item, score: 0, positions: [], basenameMatch: false });
      continue;
    }
    const whole = fuzzyMatch(query, item.label);
    const base = item.basename === undefined ? null : fuzzyMatch(query, item.basename);
    if (whole === null && base === null) {
      continue;
    }
    if (base !== null && (whole === null || base.score + 2 > whole.score)) {
      const offset = item.label.length - item.basename.length;
      scored.push({ item, score: base.score + 2, positions: base.positions.map((position) => position + offset), basenameMatch: true });
    } else {
      scored.push({ item, score: whole.score, positions: whole.positions, basenameMatch: false });
    }
  }
  scored.sort((a, b) => {
    if (query === "") {
      const ra = recency(a.item.id);
      const rb = recency(b.item.id);
      if (ra !== rb) return ra - rb;
      return a.item.label.localeCompare(b.item.label);
    }
    if (Math.abs(a.score - b.score) > 1e-9) return b.score - a.score;
    const ra = recency(a.item.id);
    const rb = recency(b.item.id);
    if (ra !== rb) return ra - rb;
    if (a.item.label.length !== b.item.label.length) return a.item.label.length - b.item.label.length;
    return a.item.label.localeCompare(b.item.label);
  });
  return scored.slice(0, limit);
}

/** The palette's command items: live, visible-on-the-palette-surface and runnable commands as `Category: Title`. */
export function commandItems(registry) {
  const items = [];
  for (const record of registry.forSurface("palette")) {
    const state = registry.describe(record.id);
    if (state === null || !state.visible || !state.runnable) {
      continue;
    }
    items.push({ id: record.id, label: `${record.category}: ${record.title}`, detail: record.description ?? "", chord: registry.keymap().chordFor(record.id) });
  }
  return items;
}

/** The palette's mode and its search text for an input value. */
export function parseInput(value) {
  if (value.startsWith(">")) {
    return { mode: "commands", text: value.slice(1).trimStart() };
  }
  if (value.startsWith(":")) {
    return { mode: "line", text: value.slice(1).trim() };
  }
  const split = /^(.*?):(\d+)\s*$/.exec(value);
  if (split !== null) {
    return { mode: "files", text: split[1].trim(), line: Number(split[2]) };
  }
  return { mode: "files", text: value.trim() };
}

/** The go-to-line target of a `:` input: `{ ok, line }` clamped to the document, or `{ ok: false, message }`. */
export function parseLine(text, lineCount) {
  if (text === "") {
    return { ok: false, message: lineCount > 0 ? `Type a line number between 1 and ${lineCount} to go to.` : "No file is open." };
  }
  if (!/^\d+$/.test(text) || lineCount === 0) {
    return { ok: false, message: lineCount > 0 ? `Not a line number. Type a number between 1 and ${lineCount}.` : "No file is open." };
  }
  const line = Number(text);
  return line >= 1 ? { ok: true, line: Math.min(line, lineCount) } : { ok: false, message: `Line numbers start at 1.` };
}

function stepOf(name, spec, required, providers) {
  const label = spec.title ?? name;
  const choices = spec.enum?.map((value) => ({ label: String(value), value, detail: "" })) ?? providerChoices(name, spec, providers);
  if (choices !== null) {
    return { name, kind: "choice", label, required, choices };
  }
  const numeric = spec.type === "integer" || spec.type === "number";
  const hint = [spec.minimum !== undefined ? `min ${spec.minimum}` : "", spec.maximum !== undefined ? `max ${spec.maximum}` : ""].filter(Boolean).join(", ");
  return { name, kind: numeric ? "number" : "text", label, required, hint, spec };
}

function providerChoices(name, spec, providers) {
  const provide = spec["x-provider"] === undefined ? undefined : providers?.[spec["x-provider"]];
  if (provide === undefined) {
    return null;
  }
  return provide()
    .filter((entry) => entry.args !== undefined && entry.args[name] !== undefined)
    .map((entry) => ({ label: entry.label, value: entry.args[name], detail: entry.detail ?? "" }));
}

function checkText(step, text) {
  const trimmed = text.trim();
  if (trimmed === "") {
    return { ok: false, message: `${step.label} is required.` };
  }
  const spec = step.spec;
  if (step.kind === "number") {
    const value = Number(trimmed);
    if (!Number.isFinite(value) || (spec.type === "integer" && !Number.isInteger(value))) {
      return { ok: false, message: `${step.label} must be ${spec.type === "integer" ? "a whole number" : "a number"}.` };
    }
    if ((spec.minimum !== undefined && value < spec.minimum) || (spec.maximum !== undefined && value > spec.maximum)) {
      return { ok: false, message: `${step.label} must be ${step.hint}.` };
    }
    return { ok: true, value };
  }
  if (spec.pattern !== undefined && !new RegExp(spec.pattern).test(trimmed)) {
    return { ok: false, message: `${step.label} is not in the expected form.` };
  }
  return { ok: true, value: trimmed };
}

/**
 * The argument prompt flow for a command with a JSON Schema `args`: one step per required property that the
 * caller did not already supply, in the schema's property order. A step with an `enum` or an `x-provider` is a
 * choice (listed, fuzzy-filterable); the rest are typed text or numbers, validated against `type`, `minimum`,
 * `maximum` and `pattern`. Pure state: `answer` and `answerText` return the next flow.
 * @param {{ id: string, title: string, args: any }} record
 * @param {Record<string, any>} given arguments already known (a menu item's literals)
 * @param {Record<string, () => Array<any>>} providers
 */
export function createArgFlow(record, given = {}, providers = {}) {
  const schema = record.args;
  const steps = [];
  if (schema?.properties) {
    const required = new Set(schema.required ?? []);
    for (const [name, spec] of Object.entries(schema.properties)) {
      if (given[name] === undefined && required.has(name)) {
        steps.push(stepOf(name, spec, true, providers));
      }
    }
  }
  const make = (index, collected) => ({
    record,
    steps,
    index,
    collected,
    done: index >= steps.length,
    step: steps[index] ?? null,
    args: { ...given, ...collected },
    /** Answers the current step with a value (a choice's value, or a validated one). */
    answer: (value) => make(index + 1, { ...collected, [steps[index].name]: value }),
    /** Validates typed text for a text or number step: `{ ok, flow }` or `{ ok: false, message }`. */
    answerText(text) {
      const result = checkText(steps[index], text);
      return result.ok ? { ok: true, flow: make(index + 1, { ...collected, [steps[index].name]: result.value }) } : result;
    },
    /** One step back (Backspace on an empty answer), or the same flow at the first step. */
    back: () => (index === 0 ? make(0, collected) : make(index - 1, Object.fromEntries(Object.entries(collected).filter(([key]) => key !== steps[index - 1].name)))),
  });
  return make(0, {});
}

/** True when running `record` with `given` still needs an answer from the person. */
export function needsPrompt(record, given = {}) {
  const required = record.args?.required ?? [];
  return required.some((name) => given[name] === undefined);
}
