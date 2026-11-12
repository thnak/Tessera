// Scans wiki-content/ and writes src/generated/manifest.json: pages (name, title, section) in
// sidebar order. Page text itself is bundled lazily by Vite; this only provides navigation.
//   _Sidebar.md  (GitHub Wiki convention) sets order and sections: a `#` heading or a `**bold**`
//                line opens a section, `[[Page]]`, `[[Text|Page]]` and `[Text](Page)` add pages.
//   Home.md      is the landing page.
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const contentDir = join(root, "wiki-content");

// Must match pageKey() in src/content.ts.
const pageKey = (name) => name.trim().replace(/\s+/g, "-").toLowerCase();

function* walk(dir) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) yield* walk(full);
    else if (name.endsWith(".md")) yield full;
  }
}

function titleOf(text, fallback) {
  let inFence = false;
  for (const line of text.split(/\r?\n/)) {
    if (/^(```|~~~)/.test(line)) inFence = !inFence;
    else if (!inFence && line.startsWith("# ")) return line.slice(2).trim();
  }
  return fallback;
}

const pages = [];
if (existsSync(contentDir)) {
  for (const file of walk(contentDir)) {
    const name = relative(contentDir, file).split(sep).join("/").replace(/\.md$/, "");
    const base = name.split("/").pop();
    if (base.startsWith("_")) continue;
    pages.push({ name, title: titleOf(readFileSync(file, "utf8"), base.replace(/-/g, " ")) });
  }
}
const byKey = new Map(pages.map((page) => [pageKey(page.name), page]));

const sections = [];
const placed = new Set();
const sidebarFile = join(contentDir, "_Sidebar.md");
if (existsSync(sidebarFile)) {
  let current = { title: "Pages", names: [] };
  const open = (title) => {
    current = { title, names: [] };
    sections.push(current);
  };
  for (const line of readFileSync(sidebarFile, "utf8").split(/\r?\n/)) {
    const heading = /^#{1,6}\s+(.+?)\s*$/.exec(line) ?? /^\*\*(.+?)\*\*\s*:?\s*$/.exec(line);
    if (heading) open(heading[1]);
    for (const match of line.matchAll(/\[\[([^\]]+)\]\]|\[[^\]]*\]\(([^)#\s]+)\)/g)) {
      const target = match[1] !== undefined ? (match[1].split("|").pop() ?? "") : decodeURI(match[2] ?? "");
      const page = byKey.get(pageKey(target.replace(/\.md$/, "")));
      if (page && !placed.has(page.name)) {
        if (!sections.includes(current)) sections.push(current);
        current.names.push(page.name);
        placed.add(page.name);
      }
    }
  }
}

const rest = pages.filter((page) => !placed.has(page.name));
rest.sort((a, b) => (a.name === "Home" ? -1 : b.name === "Home" ? 1 : a.name.localeCompare(b.name)));
if (rest.length > 0) sections.push({ title: sections.length === 0 ? "Pages" : "More", names: rest.map((p) => p.name) });

const byName = new Map(pages.map((page) => [page.name, page]));
const manifest = {
  home: byKey.has("home") ? byKey.get("home").name : (pages[0]?.name ?? null),
  sections: sections
    .filter((section) => section.names.length > 0)
    .map((section) => ({ title: section.title, pages: section.names.map((name) => byName.get(name)) })),
};

const out = join(root, "src", "generated", "manifest.json");
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, JSON.stringify(manifest, null, 1) + "\n");
console.log(`manifest: ${pages.length} wiki pages`);
