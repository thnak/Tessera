// SPDX-License-Identifier: MIT

// The migration table (studio-workbench-layout-v1.md section 2.2): `migrations[n]` turns a version-n
// document into version n+1, pure and total, with a fixture per step under
// `tests/fixtures/studio-layout/v<n>.json`. Version 2 added the Problems panel (`problems`): the 1 -> 2 step
// puts it first in the tab strip of the group that holds Diagnostics, so a stored layout reads like the new
// default instead of getting the panel appended last by `reconcile`.

import { LAYOUT_VERSION } from "./types";

export type Migration = (doc: Record<string, unknown>) => Record<string, unknown>;

/** Version 1 -> 2: `problems` joins the Diagnostics group as its first tab, unless the viewer closed it or it is placed. */
const addProblemsPanel: Migration = (doc) => {
  const next = JSON.parse(JSON.stringify(doc)) as Record<string, unknown>;
  const closed = Array.isArray(next.closed) ? (next.closed as unknown[]) : [];
  const groups: { panels?: unknown; active?: unknown }[] = [];
  const walk = (node: unknown): void => {
    if (typeof node !== "object" || node === null) {
      return;
    }
    const record = node as { type?: unknown; children?: unknown; panels?: unknown };
    if (record.type === "group") {
      groups.push(record);
    } else if (Array.isArray(record.children)) {
      record.children.forEach(walk);
    }
  };
  walk(next.root);
  const placed = groups.some((group) => Array.isArray(group.panels) && group.panels.includes("problems"));
  const host = groups.find((group) => Array.isArray(group.panels) && group.panels.includes("diagnostics"));
  if (!placed && !closed.includes("problems") && host !== undefined) {
    host.panels = ["problems", ...(host.panels as unknown[])];
  }
  return next;
};

export const migrations: Readonly<Record<number, Migration>> = { 1: addProblemsPanel };

export type MigrateResult =
  | { ok: true; doc: Record<string, unknown> }
  | { ok: false; reason: "invalid" | "newer" | "missing-step"; version: number | null };

export function migrateLayout(
  doc: unknown,
  table: Readonly<Record<number, Migration>> = migrations,
  current: number = LAYOUT_VERSION,
): MigrateResult {
  if (typeof doc !== "object" || doc === null || Array.isArray(doc)) {
    return { ok: false, reason: "invalid", version: null };
  }
  let out = doc as Record<string, unknown>;
  const version = out.version;
  if (typeof version !== "number" || !Number.isInteger(version) || version < 1) {
    return { ok: false, reason: "invalid", version: null };
  }
  if (version > current) {
    return { ok: false, reason: "newer", version };
  }
  for (let step = version; step < current; step += 1) {
    const migration = table[step];
    if (migration === undefined) {
      return { ok: false, reason: "missing-step", version };
    }
    out = { ...migration(out), version: step + 1 };
  }
  return { ok: true, doc: out };
}
