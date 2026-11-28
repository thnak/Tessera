// SPDX-License-Identifier: MIT

// The chrome's Node build, used only by the gates (ADR-054 decision 6).
//
// React creates the studio's DOM at runtime, so "every id the chrome looks up exists in index.html" is no
// longer a statement a static file check can make: the shipped `index.html` is a mount point. The
// alternative to reading the page is to render the chrome itself, and `react-dom/server`'s
// `renderToStaticMarkup` does that with no browser and no new dependency (issue #422; jsdom would need a
// dependencies.md and THIRD_PARTY_NOTICES.md entry of its own).
//
// Three entries, because the gates need three things from the same source the site ships:
//
//   ssr-chrome   the components, rendered to markup (`preview_page_checks.mjs`, `preview_studio_smoke.mjs`)
//   preferences  the stored toolbar (`preview_studio_smoke.mjs`)
//   sample-folder  a picked folder's sample entries and companion files (`preview_studio_smoke.mjs`,
//                `preview_sample_projects.mjs`)
//   skins        the skin manifest's validator and its geometry (`preview_skins.mjs`, issue #171)
//
// `react`/`react-dom` stay external so the gate resolves them from `designer/web/node_modules` rather than
// carrying a second copy. This ships nothing: `dist-ssr/` is git-ignored and is never assembled into the
// Pages artifact.
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  build: {
    ssr: true,
    outDir: "dist-ssr",
    emptyOutDir: true,
    target: "node20",
    minify: false,
    copyPublicDir: false,
    rollupOptions: {
      input: {
        "ssr-chrome": "src/ssr-chrome.tsx",
        preferences: "src/lib/preferences.ts",
        "sample-folder": "src/lib/sample-folder.ts",
        // The Problems model (severity counts, filter, store), so `scripts/ci/preview_problems.mjs` tests what ships.
        problems: "src/lib/problems.ts",
        // The project-wide Problems source (#575): the analysis entries and the answer's rows,
        // so `scripts/ci/preview_project_problems.mjs` drives what the page ships against the worker protocol.
        "project-problems": "src/lib/project-problems.ts",
        // The page-side clients of the compile and analysis workers (#569, #575), so the gates drive the code the
        // page ships (restart after a killed worker, unavailable engine) with an injected worker handle.
        "compile-client": "src/lib/compile-client.ts",
        "analysis-client": "src/lib/analysis-client.ts",
        // The editor's code intelligence (#984, #988): the answers as page data, and the hub the commands and the
        // CodeMirror extensions share, so `scripts/ci/preview_analysis_wiring.mjs` drives what the page ships.
        "analysis-nav": "src/lib/analysis-nav.ts",
        "analysis-hub": "src/editor/analysis-hub.ts",
        // The project search client and the replace history (#580), so `scripts/ci/preview_search_client.mjs` drives what ships.
        "search-client": "src/lib/search-client.ts",
        "search-replace": "src/lib/search-replace.ts",
        // The version history model (#583), so `scripts/ci/preview_history_model.mjs` drives what ships.
        "history-model": "src/lib/history-model.ts",
        // The editor tab rules (#562): preview tabs, pinning and the path breadcrumbs, for `scripts/ci/preview_editor_tabs.mjs`.
        "editor-tabs": "src/lib/editor-tabs.ts",
        // The skin manifest's rules, so `scripts/ci/preview_skins.mjs` tests the validator the page
        // ships rather than a copy of it. `skin-load.ts` is here too: the gate drives the real loader
        // against a stub `fetch`, which is what proves a missing manifest is a rejection and not a
        // crash.
        skins: "src/views/skins.ts",
        "skin-geometry": "src/views/skin-geometry.ts",
        "skin-load": "src/views/skin-load.ts",
        // The frame component itself: `preview_skins.mjs` walks its element tree to prove the canvas keeps
        // one position with and without a skin, because a changed position is a remount.
        "skin-frame": "src/regions/SkinFrame.tsx",
        // The device profile's rules and the platform-event vocabulary (issues #447 and #448), so
        // `scripts/ci/preview_profiles.mjs` and `preview_platform_events.mjs` test what the page ships.
        "device-profile": "src/lib/device-profile.ts",
        "device-profile-load": "src/lib/device-profile-load.ts",
        "platform-events": "src/lib/platform-events.ts",
        "profiles-switch": "src/page-profiles.ts",
        // The workbench layout model, operations, migration and persistence (issue #556), so
        // `scripts/ci/workbench_layout.mjs` tests the code the page ships.
        workbench: "src/workbench/layout.ts",
        // The dock adapter's pure core (codec, gesture map, splitter arithmetic), so
        // `scripts/ci/workbench_dock_core.mjs` tests what the page ships (issue #560).
        "dock-core": "src/dock/core.ts",
        // The canvas pane's state transitions (failed, not applied, nothing to draw), so
        // `scripts/ci/preview_canvas_pane_states.mjs` tests what the page ships.
        "canvas-pane": "src/views/canvas-pane.ts",
        // The pane's zoom arithmetic, so `scripts/ci/preview_canvas_zoom.mjs` tests what the page ships.
        "canvas-zoom": "src/views/canvas-zoom.ts",
        // The viewport limits and the clamp every canvas request goes through (issue #690), so
        // `scripts/ci/preview_viewport_limits.mjs` tests the shipped rule against the engine's.
        "canvas-viewport": "src/views/canvas-viewport.ts",
        // The drag-resize arithmetic (issue #690 slice b): one step per animation frame, and the same
        // clamp. `scripts/ci/preview_canvas_resize.mjs`.
        "canvas-resize": "src/views/canvas-resize.ts",
        "canvas-motion": "src/views/canvas-motion.ts",
        // The fold-hinge and safe-area overlay's geometry (issue #433): the band and the inset boxes
        // scaled from the `WindowMetrics` the engine reported. `scripts/ci/preview_fold_overlay.mjs`.
        "canvas-fold": "src/views/canvas-fold.ts",
        // The hidden text element that gives the canvas typed text and IME (issue #601) and the keyboard
        // forwarder it cooperates with. `scripts/ci/preview_canvas_text.mjs` drives both against a fake DOM.
        "canvas-text": "src/views/canvas-text.ts",
        "canvas-keyboard": "src/views/canvas-keyboard.ts",
        // The pointer forwarder; `scripts/ci/preview_canvas_text.mjs` tests its multi-press counter.
        "canvas-pointer": "src/views/canvas-pointer.ts",
        // The Resources controls' request, report and sentences (issue #657), and the page-side call that
        // drives the engine's `tessera_canvas_resource`. `scripts/ci/preview_resource_controls.mjs`.
        "resource-controls": "src/lib/resource-controls.ts",
        "canvas-resource": "src/views/canvas-resource.ts",
      },
      output: {
        format: "es",
        entryFileNames: "[name].js",
        chunkFileNames: "chunks/[name]-[hash].js",
      },
    },
  },
});
