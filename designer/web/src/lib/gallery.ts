// SPDX-License-Identifier: MIT

// The sample gallery's data half (issue #169, P5): the manifest fetch, the sample fetch and the derived
// titles, with **no DOM** so the React component can render the list and the Node smoke test can drive
// the same code.
//
// The gallery holds no sample list of its own: `samples.json` is generated from `samples/` by
// `scripts/ci/gen_sample_manifest.py` and carries a path and a component name. The TSX is fetched from
// that path at selection time, so the file in the repository stays the single source of truth and
// adding a sample never means editing TypeScript.
//
// Nothing here runs at import time and nothing here is on the first-paint path: the manifest is
// fetched the first time a visitor opens the gallery, so P6's `cold_engine_ready_ms` cannot grow
// because a page shipped a gallery.

/** The manifest schema this page reads; a newer one is a page change, not a data fix. */
export const MANIFEST_SCHEMA = 2;

/** A companion file of a sample: the repository path the page fetches and the project path it opens at. */
export interface SampleFile {
  from: string;
  as: string;
}

/**
 * One entry of the generated manifest. A sample is a small project: `path` is the entry, `files` the
 * companions its relative imports reach (the library `Button`, a CSS module), derived by the generator.
 */
export interface Sample {
  path: string;
  component: string;
  files: SampleFile[];
}

/** What a gallery pick reports: the manifest entry, the entry's TSX and the fetched companions. */
export interface LoadedSample extends Sample {
  text: string;
  /** The companion files, fetched, at their project paths (`SampleFile.as`). */
  companions: { path: string; text: string }[];
}

/** A per-page-load cache, so opening the gallery twice does not refetch the manifest. */
export interface GalleryCache {
  samples: Sample[] | null;
}

const UNSAFE_PATH = /^[a-z]+:/i;

function isUnsafePath(path: string): boolean {
  return path.startsWith("/") || path.includes("..") || path.includes("\\") || UNSAFE_PATH.test(path);
}

/**
 * Fetches and validates the sample manifest. Called lazily (the gallery's first open), never at
 * module load.
 *
 * Throws an `Error` with a readable message rather than returning a partial list: a gallery that
 * silently shows three of four samples is worse than one that says it could not load.
 */
export async function loadSampleManifest(fetchImpl: typeof fetch = globalThis.fetch): Promise<Sample[]> {
  const response = await fetchImpl("./samples.json", { cache: "no-cache" });
  if (!response.ok) {
    throw new Error(`the sample manifest is unavailable (HTTP ${response.status})`);
  }
  const document = (await response.json()) as { schema?: number; samples?: unknown };
  if (!document || document.schema !== MANIFEST_SCHEMA) {
    throw new Error(
      `the sample manifest is schema ${String(document?.schema)}, expected ${MANIFEST_SCHEMA}`,
    );
  }
  if (!Array.isArray(document.samples) || document.samples.length === 0) {
    throw new Error("the sample manifest lists no samples");
  }
  const samples: Sample[] = [];
  for (const entry of document.samples as Partial<Sample>[]) {
    if (
      !entry ||
      typeof entry.path !== "string" ||
      typeof entry.component !== "string" ||
      !Array.isArray(entry.files)
    ) {
      throw new Error("the sample manifest has an entry without a path, component and files");
    }
    // The gallery fetches these paths. An absolute path or `..` would make the manifest an injection
    // point; the generator never writes one, and a hand-edited manifest is not trusted.
    if (isUnsafePath(entry.path)) {
      throw new Error(`the sample manifest has an unsafe path: ${entry.path}`);
    }
    const files: SampleFile[] = [];
    for (const file of entry.files as Partial<SampleFile>[]) {
      if (!file || typeof file.from !== "string" || typeof file.as !== "string") {
        throw new Error(`the sample manifest has a companion file without from and as: ${entry.path}`);
      }
      for (const path of [file.from, file.as]) {
        if (isUnsafePath(path)) {
          throw new Error(`the sample manifest has an unsafe path: ${path}`);
        }
      }
      files.push({ from: file.from, as: file.as });
    }
    samples.push({ path: entry.path, component: entry.component, files });
  }
  return samples;
}

/** Fetches one sample's TSX: the sample is the source of truth, so it is read, never embedded. */
export async function loadSampleSource(
  path: string,
  fetchImpl: typeof fetch = globalThis.fetch,
): Promise<string> {
  const response = await fetchImpl("./" + path, { cache: "no-cache" });
  if (!response.ok) {
    throw new Error(`cannot read ${path} (HTTP ${response.status})`);
  }
  return response.text();
}

/**
 * Fetches a sample as a project: the entry plus every companion file its imports need. A companion
 * that cannot be read fails the whole pick, so the Studio never opens a sample that cannot compile
 * (the compiler would only say "does not name a file of this project").
 */
export async function loadSampleProject(
  sample: Sample,
  fetchImpl: typeof fetch = globalThis.fetch,
): Promise<LoadedSample> {
  const [text, ...companions] = await Promise.all([
    loadSampleSource(sample.path, fetchImpl),
    ...sample.files.map((file) => loadSampleSource(file.from, fetchImpl)),
  ]);
  return {
    ...sample,
    text,
    companions: sample.files.map((file, index) => ({ path: file.as, text: companions[index] })),
  };
}

/**
 * A display title derived from the file stem (`SafeAreaPage.tsx` -> "Safe Area Page"), qualified by
 * the sample's directory so two same-named files stay distinguishable.
 *
 * Derived rather than stored: the manifest carries no prose, so a rename cannot leave a stale title.
 */
export function titleOf(path: string): string {
  const parts = String(path).split("/");
  const file = parts[parts.length - 1] || String(path);
  const stem = file.replace(/\.tsx?$/i, "");
  const words = stem
    .replace(/[_-]+/g, " ")
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .trim();
  const title = words.replace(/\b\w/g, (character) => character.toUpperCase());
  const group = parts.length > 2 ? parts[parts.length - 2] : "";
  return group ? `${title} (${group})` : title;
}

/** The manifest, from `cache` when the page load already has it. */
export async function cachedManifest(
  cache: GalleryCache,
  fetchImpl: typeof fetch = globalThis.fetch,
): Promise<Sample[]> {
  if (!cache.samples) {
    cache.samples = await loadSampleManifest(fetchImpl);
  }
  return cache.samples;
}

const pageCache: GalleryCache = { samples: null };

/** The manifest for this page load, shared by the welcome view and the Open Sample quick pick. */
export function pageManifest(): Promise<Sample[]> {
  return cachedManifest(pageCache);
}
