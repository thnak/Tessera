// SPDX-License-Identifier: MIT

// Persistence of the layout (studio-workbench-layout-v1.md section 2.1): `localStorage` only, per viewer,
// every access wrapped for the empty/throwing case as `lib/preferences.ts` does. A layout problem is never a
// page error and never blocks boot: the default layout is the answer to every failure.

import { defaultContext, freshDefault } from "./layout-context";
import { migrateLayout } from "./migrate";
import { reconcile } from "./reconcile";
import { cloneLayout } from "./tree";
import type { Context, Layout } from "./types";
import { MAX_LAYOUT_BYTES, validateLayout } from "./validate";

export const LAYOUT_STORAGE_KEY = "tessera.studio.layout";
export const LAYOUT_BACKUP_KEY = "tessera.studio.layout.backup";
export const SAVE_DEBOUNCE_MS = 300;

export interface StorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem?(key: string): void;
}

/** `localStorage`, or `null` when the property access itself throws (a blocked profile). */
export function browserStorage(): StorageLike | null {
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

export interface LoadedLayout {
  layout: Layout;
  /** Where it came from: `stored` was read and kept; `default` is the build's default. */
  source: "stored" | "default";
  /** A status-line message when the stored layout was set aside; `null` otherwise. */
  notice: string | null;
}

function backUp(storage: StorageLike, raw: string): void {
  try {
    storage.setItem(LAYOUT_BACKUP_KEY, raw.slice(0, MAX_LAYOUT_BYTES));
  } catch {
    /* a full or blocked store: the backup is a courtesy */
  }
}

/** Reads, migrates, validates and reconciles; any failure yields the default (and a backup + notice). */
export function loadLayout(storage: StorageLike | null, ctx: Context = defaultContext): LoadedLayout {
  const fallback = (notice: string | null): LoadedLayout => ({
    layout: reconcile(freshDefault(ctx), ctx),
    source: "default",
    notice,
  });
  if (storage === null) {
    return fallback(null);
  }
  let raw: string | null = null;
  try {
    raw = storage.getItem(LAYOUT_STORAGE_KEY);
  } catch {
    return fallback(null);
  }
  if (raw === null) {
    return fallback(null);
  }
  // A document is parsed once for its version, so a newer one is kept verbatim rather than rewritten.
  let probe: unknown = null;
  if (raw.length <= MAX_LAYOUT_BYTES) {
    try {
      probe = JSON.parse(raw);
    } catch {
      probe = null;
    }
  }
  const migrated = migrateLayout(probe);
  if (!migrated.ok && migrated.reason === "newer") {
    backUp(storage, raw);
    return fallback("Layout from a newer studio; using the default");
  }
  if (!migrated.ok) {
    backUp(storage, raw);
    return fallback("The saved layout could not be read; using the default");
  }
  const checked = validateLayout(migrated.doc);
  if (!checked.ok) {
    backUp(storage, raw);
    return fallback("The saved layout could not be read; using the default");
  }
  return { layout: reconcile(checked.layout, ctx), source: "stored", notice: null };
}

/** The form that is written: zen is transient and never stored. */
export function serializeLayout(layout: Layout): string {
  const copy = cloneLayout(layout);
  copy.shell.zen = false;
  return JSON.stringify(copy);
}

export interface Timers {
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}

export interface LayoutSaver {
  /** Schedules a write 300 ms after the last call. */
  schedule(layout: Layout): void;
  /** Writes now (pagehide). */
  flush(): void;
  cancel(): void;
}

const realTimers: Timers = {
  setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
  clearTimeout: (handle) => globalThis.clearTimeout(handle as number),
};

export function createLayoutSaver(
  storage: StorageLike | null,
  timers: Timers = realTimers,
  delayMs: number = SAVE_DEBOUNCE_MS,
): LayoutSaver {
  let pending: Layout | null = null;
  let handle: unknown = null;
  const write = (): void => {
    handle = null;
    const layout = pending;
    pending = null;
    if (layout === null || storage === null) {
      return;
    }
    const text = serializeLayout(layout);
    if (text.length > MAX_LAYOUT_BYTES) {
      return;
    }
    try {
      storage.setItem(LAYOUT_STORAGE_KEY, text);
    } catch {
      /* quota or blocked: the layout is a convenience */
    }
  };
  return {
    schedule(layout) {
      pending = layout;
      if (handle !== null) {
        timers.clearTimeout(handle);
      }
      handle = timers.setTimeout(write, delayMs);
    },
    flush() {
      if (handle !== null) {
        timers.clearTimeout(handle);
      }
      write();
    },
    cancel() {
      if (handle !== null) {
        timers.clearTimeout(handle);
      }
      handle = null;
      pending = null;
    },
  };
}
