// SPDX-License-Identifier: MIT

// The migration table (studio-workbench-layout-v1.md section 2.2): `migrations[n]` turns a version-n
// document into version n+1, pure and total, with a fixture per step under
// `tests/fixtures/studio-layout/v<n>.json`. Version 1 is the first, so the table is empty today; the
// machinery is exercised by the gate with a synthetic table.

import { LAYOUT_VERSION } from "./types";

export type Migration = (doc: Record<string, unknown>) => Record<string, unknown>;

export const migrations: Readonly<Record<number, Migration>> = {};

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
