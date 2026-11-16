// SPDX-License-Identifier: MIT

// Where the three Wasm modules are, from the page's own base.
//
// The modules are **not** bundled and not in `public/`: they are build artifacts the Pages lane copies
// next to the assembled `index.html` (`.github/workflows/pages.yml`, the "Assemble site" step), together
// with `samples/` and `build-id.txt`. A relative specifier inside a built bundle would resolve against
// `assets/`, not against the document, so a `new URL(..., document.baseURI)` is what keeps a lazy
// `import()` pointed at the site root no matter which hashed asset directory Vite chose.
//
// This is also what keeps the three modules *three lazy loads* (ADR-054 decision 3): each view imports
// only its own file, on first use, and a `@vite-ignore` keeps the bundler from turning that dynamic
// import into a preloaded chunk. A browser without WebGPU therefore still pays for Diagnostics (the
// default view) and Scene (when opened) and never downloads `tessera_canvas.wasm`.

/** An absolute URL for a site-root file, resolved against the document's base. */
export function siteUrl(name: string): string {
  const base =
    typeof document !== "undefined" && document.baseURI
      ? document.baseURI
      : globalThis.location?.href ?? "http://localhost/";
  return new URL(name, base).href;
}

/**
 * The specifier for one of the three Wasm module loaders (`tessera_diagnostics.js`,
 * `tessera_scene_preview.js`, `tessera_canvas.js`).
 */
export function wasmModuleUrl(name: string): string {
  return siteUrl(name);
}
