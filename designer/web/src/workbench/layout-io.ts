// SPDX-License-Identifier: MIT

// Layout import, export and the viewer's saved presets (slice W8 #565, `studio-workbench-layout-v1.md` section 2).
//
// Pure, like the rest of the layout core. An import goes through the same migrate, validate and reconcile steps as
// a stored layout, and a refusal says why in one sentence while changing nothing. Saved presets live in
// `localStorage` beside the layout, per viewer; every access is wrapped so a blocked store only means "no presets".

import { defaultContext } from "./layout-context";
import { LAYOUT_VERSION } from "./types";
import { migrateLayout } from "./migrate";
import { reconcile } from "./reconcile";
import { cloneLayout } from "./tree";
import type { Context, Layout } from "./types";
import { MAX_LAYOUT_BYTES, validateLayout } from "./validate";
import type { StorageLike } from "./persist";

export type ImportResult = { ok: true; layout: Layout } | { ok: false; message: string };

/** The exported text: the persisted form (zen is transient) indented for reading and diffing. */
export function exportLayout(layout: Layout): string {
  const copy = cloneLayout(layout);
  copy.shell.zen = false;
  return `${JSON.stringify(copy, null, 2)}\n`;
}

/** Parses, migrates, validates and reconciles pasted or uploaded text. Never throws. */
export function importLayout(text: string, ctx: Context = defaultContext): ImportResult {
  if (text.length > MAX_LAYOUT_BYTES * 2) {
    return { ok: false, message: `The file is larger than ${MAX_LAYOUT_BYTES * 2} bytes; it is not a layout.` };
  }
  let doc: unknown;
  try {
    doc = JSON.parse(text);
  } catch {
    return { ok: false, message: "The text is not JSON, so it is not a layout." };
  }
  const migrated = migrateLayout(doc);
  if (!migrated.ok) {
    return {
      ok: false,
      message:
        migrated.reason === "newer"
          ? `This layout is version ${String(migrated.version)}, from a newer studio; this one reads up to ${LAYOUT_VERSION}.`
          : "The document is not a studio layout (no format or version).",
    };
  }
  const checked = validateLayout(migrated.doc);
  if (!checked.ok) {
    const first = checked.errors[0];
    return { ok: false, message: `The layout is not valid: ${first.message} (${first.path})${checked.errors.length > 1 ? ` and ${checked.errors.length - 1} more` : ""}.` };
  }
  const layout = reconcile(checked.layout, ctx);
  layout.shell.zen = false;
  return { ok: true, layout };
}

export const PRESETS_STORAGE_KEY = "tessera.studio.layout.presets";
export const MAX_USER_PRESETS = 12;
export const MAX_PRESET_NAME = 40;

export interface UserPreset {
  name: string;
  layout: Layout;
}

/** Reads the saved presets; entries that no longer validate are dropped, a throwing or empty store is `[]`. */
export function listUserPresets(storage: StorageLike | null, ctx: Context = defaultContext): UserPreset[] {
  if (storage === null) {
    return [];
  }
  let raw: string | null;
  try {
    raw = storage.getItem(PRESETS_STORAGE_KEY);
  } catch {
    return [];
  }
  if (raw === null || raw.length > MAX_LAYOUT_BYTES * (MAX_USER_PRESETS + 1)) {
    return [];
  }
  let doc: unknown;
  try {
    doc = JSON.parse(raw);
  } catch {
    return [];
  }
  const entries = typeof doc === "object" && doc !== null && Array.isArray((doc as { presets?: unknown }).presets) ? (doc as { presets: unknown[] }).presets : [];
  const out: UserPreset[] = [];
  for (const entry of entries.slice(0, MAX_USER_PRESETS)) {
    const record = entry as { name?: unknown; layout?: unknown };
    if (typeof record?.name !== "string" || record.name === "") {
      continue;
    }
    const imported = importLayout(JSON.stringify(record.layout ?? null), ctx);
    if (imported.ok) {
      out.push({ name: record.name, layout: imported.layout });
    }
  }
  return out;
}

function writePresets(storage: StorageLike | null, presets: readonly UserPreset[]): boolean {
  if (storage === null) {
    return false;
  }
  try {
    const body = presets.map((preset) => ({ name: preset.name, layout: JSON.parse(exportLayout(preset.layout)) as unknown }));
    storage.setItem(PRESETS_STORAGE_KEY, JSON.stringify({ version: 1, presets: body }));
    return true;
  } catch {
    return false;
  }
}

export type SaveResult = { ok: true; presets: UserPreset[] } | { ok: false; message: string };

/** Saves (or replaces, by name) a preset. A blank or over-long name, a full list or a refusing store is a message. */
export function saveUserPreset(storage: StorageLike | null, name: string, layout: Layout, ctx: Context = defaultContext): SaveResult {
  const trimmed = name.trim();
  if (trimmed === "" || trimmed.length > MAX_PRESET_NAME) {
    return { ok: false, message: `A preset name is 1 to ${MAX_PRESET_NAME} characters.` };
  }
  const existing = listUserPresets(storage, ctx);
  const others = existing.filter((preset) => preset.name !== trimmed);
  if (others.length >= MAX_USER_PRESETS) {
    return { ok: false, message: `There are already ${MAX_USER_PRESETS} saved layouts; delete one first.` };
  }
  const presets = [...others, { name: trimmed, layout: cloneLayout(layout) }];
  return writePresets(storage, presets) ? { ok: true, presets } : { ok: false, message: "The browser would not store the layout (storage is blocked or full)." };
}

/** Removes a preset by name; the list that remains. */
export function deleteUserPreset(storage: StorageLike | null, name: string, ctx: Context = defaultContext): UserPreset[] {
  const presets = listUserPresets(storage, ctx).filter((preset) => preset.name !== name);
  writePresets(storage, presets);
  return presets;
}
