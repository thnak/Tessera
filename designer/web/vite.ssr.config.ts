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
//   preferences  the toolbar, the fragment decode and the share link (`preview_studio_smoke.mjs`)
//   gallery      the lazy manifest and the sample fetch (`preview_studio_smoke.mjs`)
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
        gallery: "src/lib/gallery.ts",
        // The skin manifest's rules, so `scripts/ci/preview_skins.mjs` tests the validator the page
        // ships rather than a copy of it. `skin-load.ts` is here too: the gate drives the real loader
        // against a stub `fetch`, which is what proves a missing manifest is a rejection and not a
        // crash.
        skins: "src/views/skins.ts",
        "skin-geometry": "src/views/skin-geometry.ts",
        "skin-load": "src/views/skin-load.ts",
        // The device profile's rules and the platform-event vocabulary (issues #447 and #448), so
        // `scripts/ci/preview_profiles.mjs` and `preview_platform_events.mjs` test what the page ships.
        "device-profile": "src/lib/device-profile.ts",
        "device-profile-load": "src/lib/device-profile-load.ts",
        "platform-events": "src/lib/platform-events.ts",
        "profiles-switch": "src/page-profiles.ts",
      },
      output: {
        format: "es",
        entryFileNames: "[name].js",
        chunkFileNames: "chunks/[name]-[hash].js",
      },
    },
  },
});
