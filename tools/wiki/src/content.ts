import manifestJson from "./generated/manifest.json";

export interface WikiPage {
  name: string;
  title: string;
}

export interface WikiSection {
  title: string;
  pages: WikiPage[];
}

export const manifest = manifestJson as { home: string | null; sections: WikiSection[] };

const pages = import.meta.glob("/wiki-content/**/*.md", { query: "?raw", import: "default" }) as Record<
  string,
  () => Promise<string>
>;
const images = import.meta.glob("/wiki-content/**/*.{png,jpg,jpeg,gif,svg,webp}", {
  query: "?url",
  import: "default",
}) as Record<string, () => Promise<string>>;

// Page names compare case-insensitively with spaces treated as hyphens, like GitHub Wiki links.
// Must match pageKey() in scripts/build-manifest.mjs.
export const pageKey = (name: string): string => name.trim().replace(/\s+/g, "-").toLowerCase();

const pageByKey = new Map<string, string>();
for (const file of Object.keys(pages)) {
  const name = file.slice("/wiki-content/".length).replace(/\.md$/, "");
  pageByKey.set(pageKey(name), file);
}

export function findPage(name: string): string | undefined {
  const file = pageByKey.get(pageKey(name));
  return file === undefined ? undefined : file.slice("/wiki-content/".length).replace(/\.md$/, "");
}

export function loadPage(name: string): Promise<string> | undefined {
  const file = pageByKey.get(pageKey(name));
  return file === undefined ? undefined : pages[file]?.();
}

export function loadImage(path: string): Promise<string> | undefined {
  return images["/wiki-content/" + path]?.();
}

export const repoUrl = "https://github.com/thnak/Tessera";

// Resolves `href` relative to the directory of wiki page `from`; null when it escapes the wiki root.
export function resolveWikiPath(from: string, href: string): string | null {
  const stack = from.split("/").slice(0, -1);
  for (const part of href.split("/")) {
    if (part === "" || part === ".") continue;
    if (part === "..") {
      if (stack.length === 0) return null;
      stack.pop();
    } else {
      stack.push(part);
    }
  }
  return stack.join("/");
}

// Source-repo areas that wiki pages may link to by repo-relative path (`docs/...`, `src/...`).
const sourceAreas = ["docs/", "decisions/", "src/", "include/", "samples/", "tools/", "tests/", "designer/", "scripts/"];

export function sourceUrl(path: string): string | null {
  const clean = path.replace(/^\/+/, "");
  if (!sourceAreas.some((area) => clean.startsWith(area))) return null;
  return `${repoUrl}/blob/main/${clean}`;
}
