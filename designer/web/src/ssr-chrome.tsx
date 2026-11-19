// SPDX-License-Identifier: MIT

// The chrome, rendered once for the Node gates (ADR-054 decision 6).
//
// React creates the studio's DOM at runtime, so "every id the chrome looks up exists in index.html" stopped
// being a statement a static file check can make: the shipped `index.html` is a mount point. The answer is
// to render the chrome itself, and `renderToStaticMarkup` does that with no browser, no jsdom and no new
// dependency (issue #422).
//
// `scripts/ci/preview_page_checks.mjs` imports the built `dist-ssr/chrome.js`, asks for the markup and
// asserts the same ids and roles the page's own code resolves. That keeps the check testing the real
// components rather than a copy of their markup.
//
// This module is built by `vite.ssr.config.ts` into `dist-ssr/`, which is git-ignored and never assembled
// into the Pages artifact. Nothing here reaches a visitor.

import { renderToStaticMarkup } from "react-dom/server";
import { App } from "./App";
import { seedBootProjectForRender } from "./page-boot";

/** The studio chrome as static HTML, with every id, role and class the page's code resolves. */
export function renderChrome(): string {
  seedBootProjectForRender();
  return renderToStaticMarkup(<App />);
}

/** The chrome as a visitor first sees it: no project open, nothing seeded (the owner's 2026-11-17 direction). */
export function renderBootChrome(): string {
  seedBootProjectForRender(false);
  return renderToStaticMarkup(<App />);
}

/** The three views' panes' ids, by view: the contract `preview_page_checks.mjs` checks per pane. */
export const VIEW_IDS = {
  diagnostics: ["diag-status", "diagnostics", "diag-empty"],
  scene: ["scene-status", "nodes", "scene-empty", "scene-hint", "script", "run-input", "input-status", "cells"],
  canvas: [
    "canvas-status",
    "unsupported",
    "unsupported-heading",
    "unsupported-body",
    "canvas-frame",
    "canvas",
    "failed",
    "failed-body",
    "frame-stats",
    "stat-frame",
    "stat-draws",
    "stat-vertices",
    "stat-nodes",
    "stat-dpr",
    // Issue #387's font row: in the pane, in `preview_page_checks.mjs`'s copy of this list, and now
    // here too. It was missing from this list only, which is why the two could disagree without a
    // failure; the canvas session writes it unconditionally, so a rename must fail one of them.
    "stat-font",
    // Issue #171's picker: the toggle that arms it and the overlay the outline is drawn into. The
    // overlay is `pointer-events: none`, so it never intercepts the preview's own pointer input.
    "pick-toggle",
    "pick-overlay",
    // Issue #171's skins slice: the simulated device frame around the canvas. It is page chrome, so
    // React renders it and the ids are part of the contract `preview_page_checks.mjs` checks; the
    // frame's own children (the bars, the cutout) are `aria-hidden` decoration and carry classes
    // rather than ids, because nothing looks them up.
    "skin-frame",
  ],
} as const;
