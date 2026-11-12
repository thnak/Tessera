// Scans docs/ and decisions/ and writes src/generated/manifest.json (title, section, path).
// Content itself is bundled lazily by Vite (import.meta.glob); this only provides navigation.
import { mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
const roots = ["docs", "decisions"];

function* walk(dir) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) yield* walk(full);
    else if (name.endsWith(".md")) yield full;
  }
}

function titleOf(text, fallback) {
  const fence = /^(```|~~~)/;
  let inFence = false;
  for (const line of text.split(/\r?\n/)) {
    if (fence.test(line)) inFence = !inFence;
    else if (!inFence && line.startsWith("# ")) return line.slice(2).trim();
  }
  return fallback;
}

const entries = [];
for (const root of roots) {
  for (const file of walk(join(repoRoot, root))) {
    const path = relative(repoRoot, file).split(sep).join("/");
    const parts = path.split("/");
    const base = parts[parts.length - 1].replace(/\.md$/, "");
    const section = parts.length > 2 ? `${parts[0]}/${parts[1]}` : parts[0];
    entries.push({ path, section, title: titleOf(readFileSync(file, "utf8"), base) });
  }
}

const out = join(dirname(fileURLToPath(import.meta.url)), "..", "src", "generated", "manifest.json");
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, JSON.stringify(entries, null, 1) + "\n");
console.log(`manifest: ${entries.length} documents`);
