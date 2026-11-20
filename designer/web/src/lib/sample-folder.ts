// SPDX-License-Identifier: MIT

// Opening a sample from the person's own clone of the repository (the hosted Studio no longer bundles the
// samples: there is no server, so the files come from the folder the person picks). No DOM, no fetch: the
// page hands in the files of the picked folder and the Node gates call the same functions.
//
// A sample is a small project, not one file. `samples/gallery/Gallery.tsx` imports `./Button`, and the
// native build satisfies that by copying `packages/tessera-ui/Button.tsx` next to the sample before codegen
// (`samples/gallery/CMakeLists.txt`). The rule here is that copy: a relative import resolves to a file of the
// picked folder first and else to the same name under `packages/tessera-ui/`, **when the picked folder
// contains it** (the person opened the repository root). When it does not (the person opened
// `samples/<name>`), the import is reported with the repository path of the library file, so the person
// knows what to open or copy; nothing is fetched and no sample list is kept here.
//
// What the folder means:
//   - `tessera.json` naming `app.entry` at the root: an ordinary project, opened as it is.
//   - otherwise, the sample entries: `.tsx` files that export a capitalized component, at the top level
//     (`samples/<name>` was opened), else under `samples/<name>/` (the repository root), else one level
//     down (`samples/` itself). One entry opens directly; several are offered as a choice.

/** The component library a sample's relative imports fall back to (the CMake copy rule). */
export const LIBRARY_DIR = "packages/tessera-ui";

/** Where a person finds the samples; shown as a link, never fetched. */
export const SAMPLES_URL = "https://github.com/thnak/Tessera/tree/main/samples";

/** One text file of the picked folder at its project-relative path. */
export interface TreeFile {
  path: string;
  text: string;
}

/** A relative import that resolves to no file of the picked folder. */
export interface MissingImport {
  importer: string;
  specifier: string;
  /** The project path the import names. */
  path: string;
  /** The repository path of the library file it would be, or `null` when it leaves the sample's folder. */
  library: string | null;
}

/** The project a chosen sample entry opens as. */
export interface SampleProject {
  entry: string;
  /** The files the project opens with: the entry, what it imports, the companions and `tessera.json`. */
  files: TreeFile[];
  /** Library files copied next to the importer: the project path and the folder path they came from. */
  companions: { path: string; from: string }[];
  missing: MissingImport[];
}

const EXPORT = /^\s*export\s+(?:async\s+)?(?:function|const|class)\s+([A-Za-z_$][\w$]*)/gm;
const RELATIVE_IMPORT = /(?:\b(?:import|export)\b[^;"']*?\bfrom\s*|\bimport\s*)["'](\.{1,2}\/[^"']*)["']/gs;

function dirOf(path: string): string {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? "" : path.slice(0, slash);
}

function baseOf(path: string): string {
  return path.slice(path.lastIndexOf("/") + 1);
}

/** A POSIX `normalize(join(dir, relative))`; a path that climbs above the root keeps its leading `..`. */
function joinPath(dir: string, relative: string): string {
  const out: string[] = [];
  for (const part of [...(dir === "" ? [] : dir.split("/")), ...relative.split("/")]) {
    if (part === "" || part === ".") {
      continue;
    }
    if (part === ".." && out.length > 0 && out[out.length - 1] !== "..") {
      out.pop();
    } else {
      out.push(part);
    }
  }
  return out.join("/");
}

/** The first exported, capitalized declaration (the JSX component convention), or `null`. */
export function entryComponent(text: string): string | null {
  for (const match of text.matchAll(EXPORT)) {
    if (/^[A-Z]/.test(match[1])) {
      return match[1];
    }
  }
  return null;
}

/** The relative specifiers `text` imports or re-exports, in source order, comments ignored. */
export function relativeImports(text: string): string[] {
  const code = text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  return [...code.matchAll(RELATIVE_IMPORT)].map((match) => match[1]);
}

/** The file a specifier names: a bare `./Button` is the `.tsx` module, `./x.css` is itself. */
export function importTarget(specifier: string): string {
  return /\.[A-Za-z0-9]+$/.test(specifier) ? specifier : `${specifier}.tsx`;
}

function configEntry(files: readonly TreeFile[]): string | null {
  const config = files.find((file) => file.path === "tessera.json");
  if (config === undefined) {
    return null;
  }
  try {
    const entry = (JSON.parse(config.text) as { app?: { entry?: unknown } }).app?.entry;
    return typeof entry === "string" && entry !== "" ? entry : null;
  } catch {
    return null;
  }
}

/**
 * The sample entries of a picked folder, sorted; empty when the folder is an ordinary project (its
 * `tessera.json` names an entry) or holds no component file where a sample would be.
 */
export function sampleEntries(files: readonly TreeFile[]): string[] {
  if (configEntry(files) !== null) {
    return [];
  }
  const components = files.filter((file) => file.path.endsWith(".tsx") && entryComponent(file.text) !== null);
  const levels = [
    (path: string) => !path.includes("/"),
    (path: string) => /^samples\/[^/]+\/[^/]+$/.test(path),
    (path: string) => /^[^/]+\/[^/]+$/.test(path),
  ];
  for (const level of levels) {
    const found = components.map((file) => file.path).filter(level);
    if (found.length > 0) {
      return found.sort();
    }
  }
  return [];
}

/** The `tessera.json` that names `entry`, keeping what the folder's own file says otherwise. */
function sampleConfig(files: readonly TreeFile[], entry: string, component: string): string {
  const slug = component.replace(/[^A-Za-z0-9]+/g, "_").replace(/^[^A-Za-z]+/, "").toLowerCase() || "sample";
  let config: { schemaVersion?: unknown; app?: Record<string, unknown> } = {};
  try {
    const existing = files.find((file) => file.path === "tessera.json");
    config = existing === undefined ? {} : (JSON.parse(existing.text) as typeof config);
  } catch {
    config = {};
  }
  const app = { name: slug, namespace: slug, ...(config.app ?? {}), entry };
  return `${JSON.stringify({ schemaVersion: 1, ...config, app }, null, 2)}\n`;
}

/**
 * The project `entry` opens as: the entry, every file its relative imports reach (recursively), the library
 * files copied next to their importer, and a `tessera.json` naming the entry.
 */
export function sampleProject(files: readonly TreeFile[], entry: string): SampleProject {
  const byPath = new Map(files.map((file) => [file.path, file.text]));
  const root = dirOf(entry);
  const out = new Map<string, string>();
  const companions: { path: string; from: string }[] = [];
  const missing: MissingImport[] = [];
  const pending = [entry];
  out.set(entry, byPath.get(entry) ?? "");
  while (pending.length > 0) {
    const importer = pending.pop() as string;
    for (const specifier of relativeImports(out.get(importer) ?? "")) {
      const path = joinPath(dirOf(importer), importTarget(specifier));
      if (out.has(path)) {
        continue;
      }
      const inside = root === "" ? path : path.startsWith(`${root}/`) ? path.slice(root.length + 1) : null;
      const library = inside === null || inside.startsWith("../") ? null : `${LIBRARY_DIR}/${inside}`;
      let text = byPath.get(path);
      if (text === undefined && library !== null && byPath.has(library)) {
        text = byPath.get(library) as string;
        companions.push({ path, from: library });
      }
      if (text === undefined) {
        missing.push({ importer, specifier, path, library });
        continue;
      }
      out.set(path, text);
      if (/\.tsx?$/.test(path)) {
        pending.push(path);
      }
    }
  }
  const component = entryComponent(out.get(entry) ?? "") ?? "Sample";
  out.set("tessera.json", sampleConfig(files, entry, component));
  return {
    entry,
    files: [...out].map(([path, text]) => ({ path, text })).sort((a, b) => (a.path < b.path ? -1 : 1)),
    companions: companions.sort((a, b) => (a.path < b.path ? -1 : 1)),
    missing: missing.sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0)),
  };
}

/** One sentence per missing import, naming the file and where it lives in the repository. */
export function missingSentence(missing: readonly MissingImport[]): string {
  return missing
    .map((item) =>
      item.library === null
        ? `${item.importer} imports ${item.specifier}, which is not in the opened folder.`
        : `${item.importer} imports ${item.specifier}, which is not in the opened folder: it is ` +
          `${item.library} in the repository. Open the repository root and choose ${baseOf(item.importer)} ` +
          `(the library file is copied next to it), or copy ${item.library} next to ${baseOf(item.importer)}.`,
    )
    .join(" ");
}
