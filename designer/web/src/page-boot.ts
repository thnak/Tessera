// SPDX-License-Identifier: MIT

// How the studio starts (issue #391, owner direction 2026-11-17): it does **not** open on a fixed sample
// project. The workspace starts empty, the project bar lists the projects opened before, and with no
// history the first view is the welcome view: Open folder / Open zip and the samples.
//
// The Node gates render the chrome with `renderToStaticMarkup` and still need a tree to assert the layout
// (ADR-054 decision 6), so `ssr-chrome.tsx` asks for the seed explicitly through `seedBootProjectForRender`;
// a browser never calls it.

import { seedProject, type ProjectFile } from "./lib/project";

let seedForRender = false;

/** Makes `bootProject` return the demo project; only the server-rendered chrome check calls this. */
export function seedBootProjectForRender(seed = true): void {
  seedForRender = seed;
}

/** The project the workspace starts with: nothing, except for the Node render of the chrome. */
export function bootProject(): ProjectFile[] {
  return seedForRender ? seedProject() : [];
}

/** `true` when the page was opened with a URL fragment (a share link); the decoder otherwise yields its default source. */
export function hasShareLink(): boolean {
  return (globalThis.location?.hash ?? "") !== "";
}

/**
 * A sample as a one-file project: the sample plus the `tessera.json` that names it as the entry, so
 * the preview has something to run. It lives in memory only (a sample is never written anywhere).
 */
export function sampleProjectFiles(
  path: string,
  text: string,
  component: string,
): { path: string; text: string }[] {
  const slug = component.replace(/[^A-Za-z0-9]+/g, "_").replace(/^[^A-Za-z]+/, "").toLowerCase() || "sample";
  const config = { schemaVersion: 1, app: { name: slug, entry: path, namespace: slug } };
  return [
    { path: "tessera.json", text: `${JSON.stringify(config, null, 2)}\n` },
    { path, text },
  ];
}
