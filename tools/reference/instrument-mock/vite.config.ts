// SPDX-License-Identifier: MIT
import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

// The mock imports the library's own button.css and the repo's IBM Plex Sans subset from outside
// this folder, so the dev server may read the whole repo.
const repoRoot = fileURLToPath(new URL("../../..", import.meta.url));

export default defineConfig({
  base: "./",
  plugins: [react()],
  server: { fs: { allow: [repoRoot] } },
});
