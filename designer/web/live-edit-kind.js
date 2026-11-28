// SPDX-License-Identifier: MIT

// How the live edit classifies the file a keystroke landed in (issue #444, repaired in #865).
//
// A plain module (like `project-paths.js`) so the page and the Node gate import the same bytes.
//
// **Why this takes a reader and not a file list.** The live-edit controller is created once, on the
// first render, and outlives every render after it. A list captured when it was created is the list
// the page had *then*: since #391 the studio boots with no project, so that list is empty, and every
// keystroke in a project opened afterwards classified as "not a source file". The controller then
// took its restart branch instead of the live apply, and each edit reset the app (state cells gone,
// frame counter back to #1) instead of repainting it. The reader is called per keystroke, so it
// always sees the project that is open now.

/**
 * Returns a `kindOf(path)` for the live edit that reads the project's files at call time.
 *
 * A stylesheet is part of the previewed project, so an edit to one is a live apply like a source edit;
 * the config (it names the entry) and every other kind restart. A path with no file is "other".
 *
 * @param {() => ReadonlyArray<{ path: string; kind: string }>} files the open project's files, now
 * @returns {(path: string) => string}
 */
export function liveEditKindOf(files) {
  return (path) => {
    const file = files().find((candidate) => candidate.path === path);
    return file?.kind === "stylesheet" ? "source" : (file?.kind ?? "other");
  };
}
