import react from "@vitejs/plugin-react";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const repoRoot = fileURLToPath(new URL("../..", import.meta.url));

// Relative base keeps the static build hostable under any path (GitHub Pages project sites).
export default defineConfig({
  base: "./",
  plugins: [react()],
  server: { fs: { allow: [repoRoot] } },
});
