// SPDX-License-Identifier: MIT

// How the studio starts (issue #391, owner direction 2026-11-17): it does **not** open on a fixed sample
// project. The workspace starts empty, the project bar lists the projects opened before, and with no
// history the first view is the welcome view: Open folder / Open zip and how to open a sample.
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
