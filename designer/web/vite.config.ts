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

// The mock-service host and its transpiler (ADR-055 decision 5) are reached only through `import()`, so
// they are lazy, and they are gathered into one chunk plus the esbuild WebAssembly under `assets/mock/`.
// The directory is the contract with `scripts/preview_budget_measure.py`: everything in it is weighed by
// its own `mock_transpiler_lazy_gzip_bytes` row and left out of `download_gzip_bytes`, because a project
// without mocks never fetches it and the cold download must not move.
const MOCK_LAZY_DIR = "assets/mock";

export default defineConfig({
  base: "./",
  plugins: [react()],
  build: {
    target: "es2022",
    assetsDir: "assets",
    modulePreload: { polyfill: false },
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes("/designer/web/mock/") || id.includes("/node_modules/esbuild-wasm/")) {
            return "mock";
          }
          return undefined;
        },
        chunkFileNames: (chunk) =>
          chunk.name === "mock" ? `${MOCK_LAZY_DIR}/[name]-[hash].js` : "assets/[name]-[hash].js",
        assetFileNames: (asset) =>
          (asset.names ?? []).some((name) => name.endsWith(".wasm"))
            ? `${MOCK_LAZY_DIR}/[name]-[hash][extname]`
            : "assets/[name]-[hash][extname]",
      },
    },
  },
});
