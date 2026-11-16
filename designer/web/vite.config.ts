// SPDX-License-Identifier: MIT

// The studio page's site build (ADR-054 decision 2).
//
// `base: "./"` keeps the static build hostable under any path, including a GitHub Pages project site
// (`/Tessera/`) and the versioned `/designer/v<major>/` prefix designer-studio-v1.md section 2.1 names.
//
// **No inline script.** The page's CSP is "no inline script, no remote script" (ADR-045 decisions 1-3,
// ADR-054 decision 4), and everything Vite emits here honours it: one `<script type="module">` with a
// `src`, one `<link rel="stylesheet">` with a `href`, both same-origin and content-hashed. The
// module-preload polyfill is left off because it is the one thing that would otherwise be inlined into
// the document rather than the bundle.
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  base: "./",
  plugins: [react()],
  build: {
    target: "es2022",
    assetsDir: "assets",
    modulePreload: { polyfill: false },
  },
});
