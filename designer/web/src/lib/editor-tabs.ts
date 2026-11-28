// SPDX-License-Identifier: MIT

// The editor tab strip's pure rules (issue #562): preview (italic) tabs and the path breadcrumbs. No DOM and no
// React: `scripts/ci/preview_editor_tabs.mjs` imports this module as the page ships it.
//
// **A preview tab is a replaceable slot.** A single click on a tree row opens its file in the one preview slot;
// the next single click on another file replaces it instead of adding a tab. An edit or a double click pins it, so
// it becomes an ordinary tab. At most one tab is a preview, and a file that is already open never becomes one.

export interface TabSet {
  /** The open tabs in tab order. */
  open: readonly string[];
  /** The tab that is still a preview, or `null`. Always one of `open` when set. */
  preview: string | null;
}

/**
 * Opens `path` as a preview tab. An open file keeps its tab (and its pinned or preview state); a new one takes the
 * preview slot's place, or is appended when there is none. A dirty preview is never replaced: an edit pins it,
 * so the guard only matters for a dirty state set by another route.
 */
export function openPreview(tabs: TabSet, path: string, dirty: readonly string[] = []): TabSet {
  if (tabs.open.includes(path)) {
    return tabs;
  }
  const slot = tabs.preview;
  if (slot !== null && tabs.open.includes(slot) && !dirty.includes(slot)) {
    return { open: tabs.open.map((candidate) => (candidate === slot ? path : candidate)), preview: path };
  }
  return { open: [...tabs.open, path], preview: path };
}

/** Pins `path`: it stops being the preview tab. Any other path is a no-op. */
export function pinTab(tabs: TabSet, path: string): TabSet {
  return tabs.preview === path ? { open: tabs.open, preview: null } : tabs;
}

/** The tab set after `path` was closed. */
export function closeTabIn(tabs: TabSet, path: string): TabSet {
  return {
    open: tabs.open.filter((candidate) => candidate !== path),
    preview: tabs.preview === path ? null : tabs.preview,
  };
}

export interface Crumb {
  /** The segment as shown: a directory name, or the file name last. */
  name: string;
  /** The project path up to and including this segment. */
  path: string;
  /** True for the file itself. */
  file: boolean;
}

/** The breadcrumb segments of a project path: `ui/App.tsx` is `ui` then `App.tsx`. Symbols arrive with the analysis service. */
export function breadcrumbs(path: string): Crumb[] {
  const parts = path.split("/").filter((part) => part !== "");
  return parts.map((name, index) => ({
    name,
    path: parts.slice(0, index + 1).join("/"),
    file: index === parts.length - 1,
  }));
}
