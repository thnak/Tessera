// SPDX-License-Identifier: MIT

// What a preview run is handed: the whole project and the entry it starts from.
//
// The entry is the project's own statement, `tessera.json` `app.entry`, and nothing else. There is no
// "first source file" fallback and no "the file in the editor" fallback: a project that names no entry
// previews nothing and says why, because a preview that quietly picked another file would show a screen
// the app does not have. The modules report the same fact (`diagnose_project` "names no entry file").
//
// No DOM, no React: the page and the Node gates both call this.

import type { ProjectFile } from "./project";
import type { ProjectSnapshot } from "./views";
import type { ThemeHandover } from "./wasm-call";

/** The entry a project names, or `null` plus the reason it names none. */
export interface EntryResult {
  path: string | null;
  /** Why there is no entry; `null` when there is one. */
  problem: string | null;
}

/** Reads `app.entry` from the project's `tessera.json`, checking it is one of the project's sources. */
export function entryOf(
  files: readonly ProjectFile[],
  textOf: (path: string) => string,
): EntryResult {
  const config = files.find((file) => file.path === "tessera.json");
  if (config === undefined) {
    return { path: null, problem: "the project has no tessera.json, so it names no entry file" };
  }
  let entry: unknown;
  try {
    const parsed = JSON.parse(textOf(config.path)) as { app?: { entry?: unknown } };
    entry = parsed.app?.entry;
  } catch {
    return { path: null, problem: "tessera.json is not valid JSON, so it names no entry file" };
  }
  if (typeof entry !== "string" || entry === "") {
    return { path: null, problem: "tessera.json names no entry file (app.entry)" };
  }
  const found = files.find((file) => file.path === entry && file.kind === "source");
  if (found === undefined) {
    return { path: null, problem: `the entry file ${entry} is not a source file of the project` };
  }
  return { path: found.path, problem: null };
}

/**
 * The theme asset `tessera.json` `assets.theme` names, for `token` commits, or `null` when there is
 * none to edit: no config, no `assets.theme`, a path that is not a project file, or a document that is
 * not a theme asset (`themes` object).
 *
 * The theme id is the asset's `start` (default `default`), the theme the app opens in. The toolbar's
 * light/dark choice is a colour scheme, not a theme id of the asset, so it cannot name the object a
 * commit writes; a `start` the asset does not define falls back to its first theme rather than to a
 * theme that is not there.
 */
export function themeOf(
  files: readonly ProjectFile[],
  textOf: (path: string) => string,
): ThemeHandover | null {
  if (files.find((file) => file.path === "tessera.json") === undefined) {
    return null;
  }
  let path: unknown;
  try {
    path = (JSON.parse(textOf("tessera.json")) as { assets?: { theme?: unknown } }).assets?.theme;
  } catch {
    return null;
  }
  if (typeof path !== "string" || files.find((file) => file.path === path) === undefined) {
    return null;
  }
  const text = textOf(path);
  let asset: { themes?: unknown; start?: unknown };
  try {
    asset = JSON.parse(text) as { themes?: unknown; start?: unknown };
  } catch {
    // Malformed while the developer is typing: the engine's own patch answers a precise reason, so the
    // text is still handed over rather than hidden here.
    return { path, text, themeId: "default" };
  }
  if (typeof asset.themes !== "object" || asset.themes === null) {
    return null;
  }
  const ids = Object.keys(asset.themes);
  const start = typeof asset.start === "string" ? asset.start : "default";
  return { path, text, themeId: ids.includes(start) || ids.length === 0 ? start : ids[0] };
}

/** The snapshot of `files` as the buffers hold them, for the sessions to stage into a module. */
export function projectSnapshot(
  files: readonly ProjectFile[],
  textOf: (path: string) => string,
  active: string,
): ProjectSnapshot {
  const entry = entryOf(files, textOf);
  const theme = themeOf(files, textOf);
  return {
    ...(theme === null ? {} : { theme }),
    entry: entry.path,
    sources: files
      .filter((file) => file.kind === "source")
      .map((file) => ({ path: file.path, text: textOf(file.path) })),
    stylesheets: files
      .filter((file) => file.kind === "stylesheet")
      .map((file) => ({ path: file.path, text: textOf(file.path) })),
    active,
  };
}
