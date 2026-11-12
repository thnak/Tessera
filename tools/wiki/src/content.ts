import manifestJson from "./generated/manifest.json";

export interface DocEntry {
  path: string;
  section: string;
  title: string;
}

export const manifest = manifestJson as DocEntry[];

const loaders = import.meta.glob(["../../../docs/**/*.md", "../../../decisions/**/*.md"], {
  query: "?raw",
  import: "default",
}) as Record<string, () => Promise<string>>;

const imageLoaders = import.meta.glob("../../../docs/**/*.{png,jpg,jpeg,gif,svg,webp}", {
  query: "?url",
  import: "default",
}) as Record<string, () => Promise<string>>;

const repoPrefix = "../../../";

export function loadDoc(path: string): Promise<string> | undefined {
  return loaders[repoPrefix + path]?.();
}

export function loadImage(path: string): Promise<string> | undefined {
  return imageLoaders[repoPrefix + path]?.();
}

export function hasDoc(path: string): boolean {
  return repoPrefix + path in loaders;
}

export const repoBlobBase = "https://github.com/thnak/Tessera/blob/main/";

// Resolves `href` relative to the directory of repo-relative `from`; returns null when it escapes the repo.
export function resolveRepoPath(from: string, href: string): string | null {
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
