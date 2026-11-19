// SPDX-License-Identifier: MIT
//
// Every gallery sample, loaded the way the Studio loads it, is a project that resolves (issue: "source not
// applied: samples/gallery/Gallery.tsx: import `./Button` does not name a file of this project").
//
//   node scripts/ci/preview_sample_projects.mjs [--codegen PATH_TO_tessera_codegen]
//
// A sample is a small project: the entry plus the companion files its relative imports reach (the library
// `Button`, a CSS module). `gallery.ts` `loadSampleProject` fetches them from the manifest; this gate drives
// that same code over every manifest entry with a `fetch` that reads the repository, assembles the files at
// the project paths the Studio opens them at, and then checks:
//
//   1. **Import resolution** (always): every relative import of every assembled `.tsx` names a file of the
//      assembled project. This is the compiler's own rule (`./X` is `X.tsx` next to the importer, `./x.css`
//      is itself), restated so the gate runs without a built compiler.
//   2. **The real compiler** (when `--codegen` or `TESSERA_CODEGEN` names a native `tessera_codegen`): the
//      assembled project is written to a temporary directory and `--diagnostics-json` is run on the entry;
//      no diagnostic may say an import does not name a file, and an unrelated error is reported too.
//
// Needs the SSR build for `gallery.js` (`npm run build --prefix designer/web`), like the studio smoke.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, posix, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const GALLERY = resolve(WEB, "dist-ssr", "gallery.js");

let failures = 0;
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

let gallery;
try {
  gallery = await import(pathToFileURL(GALLERY).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: the built chrome is missing (${GALLERY}). Build it first:\n` +
      "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
  );
  process.exit(2);
}

// A `fetch` that serves the published site's layout from the checkout: `./samples.json` from the Vite public
// directory, every other `./<path>` from the repository root (the Pages lane copies exactly those files).
const fetchImpl = async (url) => {
  const path = url.replace(/^\.\//, "");
  const file = path === "samples.json" ? resolve(WEB, "public", "samples.json") : resolve(REPO_ROOT, path);
  try {
    const text = readFileSync(file, "utf8");
    return { ok: true, status: 200, json: async () => JSON.parse(text), text: async () => text };
  } catch {
    return { ok: false, status: 404, json: async () => ({}), text: async () => "" };
  }
};

const IMPORT =
  /(?:\b(?:import|export)\b[^;"']*?\bfrom\s*|\bimport\s*)["'](\.{1,2}\/[^"']*)["']/gs;

function importsOf(text) {
  const code = text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  return [...code.matchAll(IMPORT)].map((match) => match[1]);
}

/** The project the Studio opens for a sample: tessera.json, the entry and the companions. */
function assemble(loaded) {
  const files = new Map([[loaded.path, loaded.text]]);
  for (const companion of loaded.companions) {
    files.set(companion.path, companion.text);
  }
  return files;
}

const manifest = await gallery.loadSampleManifest(fetchImpl);
// Absolute, because the compiler runs with `cwd` set to the assembled project: a repository-relative
// `--codegen build/...` (what the Pages lane passes) would not resolve from there.
const codegenArgument = argument("--codegen") ?? process.env.TESSERA_CODEGEN;
const codegen = codegenArgument ? resolve(REPO_ROOT, codegenArgument) : undefined;

await test("the manifest lists samples and the strict reader accepts it", async () => {
  assert.ok(manifest.length > 0, "the manifest lists no samples");
});

for (const sample of manifest) {
  await test(`${sample.path}: every relative import names a file of the assembled project`, async () => {
    const loaded = await gallery.loadSampleProject(sample, fetchImpl);
    const files = assemble(loaded);
    const unresolved = [];
    for (const [path, text] of files) {
      if (!path.endsWith(".tsx")) {
        continue;
      }
      for (const specifier of importsOf(text)) {
        const bare = /\.[A-Za-z0-9]+$/.test(specifier) ? specifier : `${specifier}.tsx`;
        const target = posix.normalize(posix.join(posix.dirname(path), bare));
        if (!files.has(target)) {
          unresolved.push(`${path}: import \`${specifier}\` does not name a file of this project`);
        }
      }
    }
    assert.deepEqual(unresolved, []);
  });

  if (codegen) {
    await test(`${sample.path}: the compiler reports no unresolved import`, async () => {
      const loaded = await gallery.loadSampleProject(sample, fetchImpl);
      const dir = mkdtempSync(join(tmpdir(), "tessera-sample-"));
      try {
        const files = assemble(loaded);
        for (const [path, text] of files) {
          mkdirSync(dirname(join(dir, path)), { recursive: true });
          writeFileSync(join(dir, path), text);
        }
        // Relative POSIX paths from the project root: the CLI hashes ids from the path it is given, and a
        // Windows absolute path would not match `--root`.
        const inputs = [...files.keys()].filter((path) => path.endsWith(".tsx"));
        inputs.sort((left, right) => (left === loaded.path ? -1 : right === loaded.path ? 1 : 0));
        const run = spawnSync(codegen, ["--diagnostics-json", "diagnostics.json", "--root", ".", ...inputs], {
          encoding: "utf8",
          cwd: dir,
        });
        assert.ok(
          run.status === 0 || run.status === 1,
          `the compiler failed to run (${codegen}): ${run.error ? run.error.message : run.stderr}`,
        );
        const document = JSON.parse(readFileSync(join(dir, "diagnostics.json"), "utf8"));
        const messages = (document.diagnostics || []).map((entry) => `${entry.code}: ${entry.message}`);
        assert.deepEqual(
          messages.filter((message) => /does not name a file|STYLESHEET_MISSING|UNRESOLVED/.test(message)),
          [],
        );
        assert.deepEqual(messages, [], "the sample compiles without diagnostics");
      } finally {
        rmSync(dir, { recursive: true, force: true });
      }
    });
  }
}

if (!codegen) {
  process.stdout.write("note: no --codegen / TESSERA_CODEGEN; the compiler half did not run\n");
}
process.stdout.write(
  `\n${failures === 0 ? "sample projects: all cases passed" : `${failures} failed`}\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
