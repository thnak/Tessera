// SPDX-License-Identifier: MIT

// The command registry (`studio-workbench-v1.md` section 2): one record per user-invokable action, one
// handler per live command, and `run(id, args)` as the only way anything happens. Menus, the palette,
// context menus and keyboard shortcuts reference an id and carry no behaviour.
//
// This module is data plus pure logic: no DOM, no React, no CodeMirror, no dock library (a check in
// `scripts/ci/preview_workbench.mjs` enforces the import rule). The page registers handlers at boot.
//
// Results are plain data so a Tessera view can consume them: `{ ok: true }` or
// `{ ok: false, code, message }`. Codes: `unknown`, `removed`, `reserved`, `no-handler`, `disabled`,
// `invalid-args`, `failed` (the handler threw; logged and isolated).

import { clauseError, clauseKeys, evaluate } from "./when.js";
import { isKnownContextKey } from "./context.js";
import { chordProblem, createKeymap } from "./keys.js";

const ID_PATTERN = /^[a-z][a-z0-9]*(\.[a-z][A-Za-z0-9]*){1,2}$/;
const STATUSES = new Set(["live", "reserved", "removed"]);
const SURFACES = new Set(["menu", "palette", "keybinding", "toolbar", "mcp"]);
/** Services that run in a worker and report readiness (ADR-060); page-side services are always ready. */
const WORKER_SERVICES = new Set(["viewer", "analysis", "format", "mock"]);
const CLAUSE_FIELDS = ["enabled", "visible", "checked"];

/**
 * Problems in a command-record list, as strings; empty means valid. Pure, so the Node gate and the
 * registry constructor share it.
 * @param {Array<any>} records
 * @returns {string[]}
 */
export function validateRecords(records) {
  const problems = [];
  const seen = new Set();
  const ids = new Set(records.map((record) => record?.id));
  for (const record of records) {
    const id = record?.id;
    if (typeof id !== "string" || !ID_PATTERN.test(id)) {
      problems.push(`bad command id '${id}' (expected area.verbObject)`);
      continue;
    }
    if (seen.has(id)) {
      problems.push(`duplicate command id '${id}'`);
    }
    seen.add(id);
    if (typeof record.title !== "string" || record.title === "") {
      problems.push(`${id}: missing title`);
    }
    if (typeof record.category !== "string" || record.category === "") {
      problems.push(`${id}: missing category`);
    }
    if (!STATUSES.has(record.status)) {
      problems.push(`${id}: bad status '${record.status}'`);
    }
    if (record.status === "removed" && typeof record.replacedBy === "string" && !ids.has(record.replacedBy)) {
      problems.push(`${id}: removed command names unknown replacedBy '${record.replacedBy}'`);
    }
    if (record.status === "removed" && record.replacedBy === undefined) {
      problems.push(`${id}: removed command needs replacedBy (an id, or null)`);
    }
    // `titleKey` names a string context key whose non-empty value replaces the title in menus (the Edit
    // menu's "Undo Set color on Button"); the palette and the table keep the plain title.
    if (record.titleKey !== undefined && !isKnownContextKey(record.titleKey)) {
      problems.push(`${id}: titleKey names unknown context key '${record.titleKey}'`);
    }
    for (const field of CLAUSE_FIELDS) {
      const clause = record[field];
      if (clause === null || clause === undefined) {
        continue;
      }
      const error = clauseError(clause);
      if (error !== null) {
        problems.push(`${id}: ${field} clause '${clause}': ${error}`);
        continue;
      }
      for (const key of clauseKeys(clause)) {
        if (!isKnownContextKey(key)) {
          problems.push(`${id}: ${field} clause names unknown context key '${key}'`);
        }
      }
    }
    const binding = record.keybinding;
    if (binding !== null && binding !== undefined) {
      for (const chord of [binding.default, ...(binding.also ?? [])]) {
        const problem = chordProblem(chord);
        if (problem !== null) {
          problems.push(`${id}: keybinding ${problem}`);
        }
      }
      if (binding.when !== undefined && binding.when !== null) {
        const error = clauseError(binding.when);
        if (error !== null) {
          problems.push(`${id}: keybinding when '${binding.when}': ${error}`);
        } else {
          for (const key of clauseKeys(binding.when)) {
            if (!isKnownContextKey(key)) {
              problems.push(`${id}: keybinding when names unknown context key '${key}'`);
            }
          }
        }
      }
    }
    for (const surface of record.surfaces ?? []) {
      if (!SURFACES.has(surface)) {
        problems.push(`${id}: unknown surface '${surface}'`);
      }
    }
    if (record.args !== null && record.args !== undefined && record.args.type !== "object") {
      problems.push(`${id}: args must be null or a JSON Schema object`);
    }
  }
  return problems;
}

/**
 * The integrity check between records and handlers: a live command without a handler, a handler without a
 * record, and a reserved or removed command with a handler are all failures.
 * @param {Array<any>} records
 * @param {Iterable<string>} handlerIds
 */
export function checkIntegrity(records, handlerIds) {
  const problems = [];
  const handlers = new Set(handlerIds);
  const byId = new Map(records.map((record) => [record.id, record]));
  for (const record of records) {
    if (record.status === "live" && !handlers.has(record.id)) {
      problems.push(`live command '${record.id}' has no handler`);
    }
    if (record.status !== "live" && handlers.has(record.id)) {
      problems.push(`${record.status} command '${record.id}' has a handler (flip its status in the same change)`);
    }
  }
  for (const id of handlers) {
    if (!byId.has(id)) {
      problems.push(`handler '${id}' has no command record`);
    }
  }
  return problems;
}

function argProblem(schema, args) {
  if (schema === null || schema === undefined) {
    return null;
  }
  const given = args ?? {};
  if (typeof given !== "object" || Array.isArray(given)) {
    return "args must be an object";
  }
  for (const name of schema.required ?? []) {
    if (given[name] === undefined) {
      return `missing argument '${name}'`;
    }
  }
  for (const [name, spec] of Object.entries(schema.properties ?? {})) {
    const value = given[name];
    if (value === undefined) {
      continue;
    }
    const type = spec.type;
    const ok =
      type === undefined ||
      (type === "integer" ? Number.isInteger(value) : type === "number" ? typeof value === "number" && Number.isFinite(value) : typeof value === type);
    if (!ok) {
      return `argument '${name}' must be ${type}`;
    }
    if (spec.enum !== undefined && !spec.enum.includes(value)) {
      return `argument '${name}' must be one of ${spec.enum.join(", ")}`;
    }
    if (typeof value === "number" && ((spec.minimum !== undefined && value < spec.minimum) || (spec.maximum !== undefined && value > spec.maximum))) {
      return `argument '${name}' is out of range`;
    }
  }
  return null;
}

/**
 * Creates a registry.
 * @param {Array<any>} records the parsed `commands.json` `commands` array
 * @param {{ context: ReturnType<typeof import('./context.js').createContext>, log?: (level: string, event: string, detail?: object) => void, platform?: 'win'|'mac', overrides?: Array<any> }} options
 */
export function createRegistry(records, options) {
  const problems = validateRecords(records);
  if (problems.length > 0) {
    throw new Error(`invalid command records:\n  ${problems.join("\n  ")}`);
  }
  const { context, log = () => {}, platform = "win" } = options;
  const byId = new Map(records.map((record) => [record.id, record]));
  const handlers = new Map();
  const listeners = new Set();
  let overrides = options.overrides ?? [];
  let keymap = null;
  let version = 0;

  const notify = () => {
    keymap = null;
    version += 1;
    for (const listener of [...listeners]) {
      listener();
    }
  };

  /**
   * The state of a command for a surface: enabled, visible, checked, and why it cannot run. `overrides` patch
   * the context for this one answer (a menu built mid-render, before the page published its new state).
   */
  function describe(id, args, overrides) {
    const record = byId.get(id);
    if (record === undefined) {
      return null;
    }
    const base = overrides === undefined ? context.snapshot() : { ...context.snapshot(), ...overrides };
    const scope = args === undefined || args === null ? base : { ...base, ...Object.fromEntries(Object.entries(args).map(([key, value]) => [`arg.${key}`, value])) };
    let reason = "";
    if (record.status !== "live") {
      reason = record.status;
    } else if (!handlers.has(id)) {
      reason = "no handler";
    } else {
      const notReady = (record.services ?? []).find((name) => WORKER_SERVICES.has(name) && scope[`service.${name}.ready`] === false);
      if (notReady !== undefined) {
        reason = `${notReady} service is not ready`;
      } else if (record.enabled && !evaluate(record.enabled, scope)) {
        reason = "unavailable right now";
      }
    }
    const dynamicTitle = record.titleKey === undefined ? "" : scope[record.titleKey];
    return {
      record,
      // What a menu shows: the context-supplied title when the record has a `titleKey` and the key holds text.
      title: typeof dynamicTitle === "string" && dynamicTitle !== "" ? dynamicTitle : record.title,
      runnable: reason === "",
      enabled: reason === "",
      reason,
      visible: record.status === "live" && handlers.has(id) && (record.visible ? evaluate(record.visible, scope) : true),
      checked: record.checked ? Boolean(evaluate(record.checked, scope)) : null,
    };
  }

  const registry = {
    records,
    context,
    platform,
    get(id) {
      return byId.get(id);
    },
    ids: () => records.map((record) => record.id),
    handlerIds: () => [...handlers.keys()],
    /**
     * Registers the handler of a live command. Throws on a programming error (unknown id, not live, twice):
     * the integrity tests would fail the same case. Returns the unregister function.
     */
    register(id, handler) {
      const record = byId.get(id);
      if (record === undefined) {
        throw new Error(`register: no command record '${id}'`);
      }
      if (record.status !== "live") {
        throw new Error(`register: command '${id}' is ${record.status}; flip its status when its handler lands`);
      }
      if (handlers.has(id)) {
        throw new Error(`register: command '${id}' already has a handler`);
      }
      handlers.set(id, handler);
      notify();
      return () => {
        if (handlers.get(id) === handler) {
          handlers.delete(id);
          notify();
        }
      };
    },
    integrity: () => checkIntegrity(records, handlers.keys()),
    describe,
    /** Records of a surface (`menu`, `palette`, `keybinding`, `toolbar`), live ones only, in declaration order. */
    forSurface(surface) {
      return records.filter((record) => record.status === "live" && (record.surfaces ?? ["menu", "palette", "keybinding"]).includes(surface));
    },
    /**
     * Runs a command. Never throws and never rejects.
     * @returns {Promise<{ ok: true, value?: unknown } | { ok: false, code: string, message: string }>}
     */
    async run(id, args) {
      const record = byId.get(id);
      if (record === undefined) {
        return { ok: false, code: "unknown", message: `no command '${id}'` };
      }
      if (record.status === "removed") {
        const hint = record.replacedBy ? `; use '${record.replacedBy}'` : "";
        return { ok: false, code: "removed", message: `command '${id}' was removed${hint}` };
      }
      if (record.status === "reserved") {
        return { ok: false, code: "reserved", message: `command '${record.id}' is reserved for a later slice` };
      }
      const handler = handlers.get(record.id);
      if (handler === undefined) {
        return { ok: false, code: "no-handler", message: `command '${record.id}' has no handler` };
      }
      const state = describe(record.id, args);
      if (!state.runnable) {
        return { ok: false, code: "disabled", message: `${record.title} is unavailable: ${state.reason}` };
      }
      const invalid = argProblem(record.args, args);
      if (invalid !== null) {
        return { ok: false, code: "invalid-args", message: `${record.id}: ${invalid}` };
      }
      try {
        const value = await handler(args ?? {}, { id: record.id });
        log("info", "command.run", { id: record.id });
        return value === undefined ? { ok: true } : { ok: true, value };
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        log("error", "command.failed", { id: record.id, message });
        return { ok: false, code: "failed", message: `${record.title} failed: ${message}` };
      }
    },
    /** The keymap for the current records, overrides and handlers (rebuilt lazily after a change). */
    keymap() {
      if (keymap === null) {
        keymap = createKeymap({ commands: records, overrides, platform, isActive: (id) => handlers.has(id) });
      }
      return keymap;
    },
    setOverrides(next) {
      overrides = next;
      notify();
    },
    /** A counter that changes whenever the handlers, overrides or context change (a store snapshot). */
    version: () => version,
    subscribe(listener) {
      listeners.add(listener);
      const stop = context.subscribe(() => {
        version += 1;
        listener();
      });
      return () => {
        listeners.delete(listener);
        stop();
      };
    },
  };
  return registry;
}
