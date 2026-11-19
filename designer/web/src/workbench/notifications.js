// SPDX-License-Identifier: MIT

// The notifications service (issue #559; `studio-workbench-v1.md` section 6, "Notifications / toasts"): one
// place a command, a service or a page region says something to the person, instead of a `role="status"` line
// per region. Pure state with an injected clock, so the Node gate drives expiry and the React host only draws.
//
// Rules:
//  * A notification has a message, a severity (`info`, `warning`, `error`) and optional actions
//    (`{ label, command, args? }`, run through the command registry like any other chord or menu item).
//  * At most `maxVisible` toasts show; later ones wait in a queue and appear as earlier ones expire or are
//    dismissed. `info` and `warning` expire (5 s and 8 s, at least 10 s when there is an action); an `error`
//    stays until dismissed.
//  * The same `key` (default: severity and message) inside the dedupe window bumps a counter on the existing
//    entry instead of adding another, and brings its toast back if it had gone.
//  * Every notification is also kept in the history (newest first, capped) with an unread flag the bell shows.
//  * Do-not-disturb suppresses toasts (shown and queued) but not the history or the unread count.

const SEVERITIES = new Set(["info", "warning", "error"]);
const MAX_MESSAGE = 500;

/**
 * @param {{ now?: () => number, maxVisible?: number, historyLimit?: number, dedupeMs?: number, timeouts?: { info: number, warning: number, actionMinimum: number } }} [options]
 */
export function createNotifications(options = {}) {
  const { now = () => Date.now(), maxVisible = 3, historyLimit = 50, dedupeMs = 10_000 } = options;
  const timeouts = { info: 5000, warning: 8000, actionMinimum: 10_000, ...options.timeouts };
  let nextId = 1;
  let version = 0;
  let toasts = [];
  let queue = [];
  let history = [];
  let dnd = false;
  const listeners = new Set();

  const emit = () => {
    version += 1;
    for (const listener of [...listeners]) {
      listener();
    }
  };

  const expiryOf = (entry, at) => {
    if (entry.severity === "error") {
      return null;
    }
    const base = timeouts[entry.severity];
    return at + (entry.actions.length > 0 ? Math.max(base, timeouts.actionMinimum) : base);
  };

  function show(entry, at) {
    const toast = { id: entry.id, expiresAt: expiryOf(entry, at) };
    if (toasts.length < maxVisible) {
      toasts.push(toast);
    } else {
      queue.push(toast);
    }
  }

  function promote() {
    const at = now();
    while (toasts.length < maxVisible && queue.length > 0) {
      const next = queue.shift();
      const entry = history.find((candidate) => candidate.id === next.id);
      toasts.push({ id: next.id, expiresAt: entry === undefined ? null : expiryOf(entry, at) });
    }
  }

  const active = (id) => toasts.some((toast) => toast.id === id) || queue.some((toast) => toast.id === id);

  const service = {
    /**
     * Adds (or bumps) a notification and returns its id. A malformed call returns `null` and changes nothing.
     * @param {{ message: string, severity?: string, actions?: Array<{ label: string, command: string, args?: object }>, key?: string }} input
     */
    notify(input) {
      if (typeof input?.message !== "string" || input.message.trim() === "") {
        return null;
      }
      const severity = input.severity ?? "info";
      if (!SEVERITIES.has(severity)) {
        return null;
      }
      const message = input.message.trim().slice(0, MAX_MESSAGE);
      const actions = (input.actions ?? []).filter((action) => typeof action?.label === "string" && typeof action?.command === "string").slice(0, 3);
      const key = input.key ?? `${severity}:${message}`;
      const at = now();
      const existing = history.find((entry) => entry.key === key && at - entry.at <= dedupeMs);
      if (existing !== undefined) {
        existing.count += 1;
        existing.at = at;
        existing.message = message;
        existing.actions = actions;
        existing.read = false;
        history = [existing, ...history.filter((entry) => entry !== existing)];
        if (!dnd) {
          toasts = toasts.filter((toast) => toast.id !== existing.id);
          queue = queue.filter((toast) => toast.id !== existing.id);
          show(existing, at);
        }
        emit();
        return existing.id;
      }
      const entry = { id: nextId, key, message, severity, actions, count: 1, at, read: false };
      nextId += 1;
      history = [entry, ...history].slice(0, historyLimit);
      if (!dnd) {
        show(entry, at);
      }
      emit();
      return entry.id;
    },
    /** Hides a toast (it stays in the history). */
    dismiss(id) {
      const before = toasts.length + queue.length;
      toasts = toasts.filter((toast) => toast.id !== id);
      queue = queue.filter((toast) => toast.id !== id);
      promote();
      if (toasts.length + queue.length !== before) {
        emit();
      }
    },
    dismissAll() {
      if (toasts.length + queue.length > 0) {
        toasts = [];
        queue = [];
        emit();
      }
    },
    /** Expires due toasts and promotes queued ones. Returns whether anything changed. */
    tick() {
      const at = now();
      const kept = toasts.filter((toast) => toast.expiresAt === null || toast.expiresAt > at);
      if (kept.length === toasts.length) {
        return false;
      }
      toasts = kept;
      promote();
      emit();
      return true;
    },
    markRead() {
      if (history.some((entry) => !entry.read)) {
        for (const entry of history) {
          entry.read = true;
        }
        emit();
      }
    },
    clearHistory() {
      if (history.length > 0 || toasts.length > 0 || queue.length > 0) {
        history = [];
        toasts = [];
        queue = [];
        emit();
      }
    },
    setDoNotDisturb(on) {
      if (dnd === Boolean(on)) {
        return;
      }
      dnd = Boolean(on);
      if (dnd) {
        toasts = [];
        queue = [];
      }
      emit();
    },
    /** A copy of the state for a renderer: visible toasts and queued count, the history, unread, do-not-disturb. */
    snapshot() {
      const byId = new Map(history.map((entry) => [entry.id, entry]));
      return {
        version,
        dnd,
        unread: history.filter((entry) => !entry.read).length,
        queued: queue.length,
        toasts: toasts.map((toast) => ({ ...byId.get(toast.id), expiresAt: toast.expiresAt })).filter((toast) => toast.id !== undefined),
        history: history.map((entry) => ({ ...entry, active: active(entry.id) })),
      };
    },
    version: () => version,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
  return service;
}
