// SPDX-License-Identifier: MIT
//
// Every sample, opened the way the Studio opens it from a clone of the repository, is a project that resolves
// (issue: "source not applied: samples/gallery/Gallery.tsx: import `./Button` does not name a file of this
// project").
//
//   node scripts/ci/preview_sample_projects.mjs [--codegen PATH_TO_tessera_codegen]
//
// The hosted Studio bundles no samples: a visitor clones the repository and opens the repository root or
// `samples/<name>` (`designer/web/src/lib/sample-folder.ts`). This gate drives that same code over this
// checkout, as if the repository root were picked (the files under `samples/` and `packages/tessera-ui/`), and
// checks for every sample entry it finds:
//
//   1. **Import resolution** (always): no relative import is missing, and every relative import of every
//      assembled `.tsx` names a file of the assembled project. This is the compiler's own rule (`./X` is
//      `X.tsx` next to the importer, `./x.css` is itself), restated so the gate runs without a compiler.
//   2. **The `samples/<name>` diagnostic** (always): opened alone, a sample's missing imports each name a
//      library file that exists in the repository, so the advice the Studio prints is true.
//   3. **The real compiler** (when `--codegen` or `TESSERA_CODEGEN` names a native `tessera_codegen`): the
//      assembled project is written to a temporary directory and `--diagnostics-json` is run on the entry;
//      no diagnostic may say an import does not name a file, and an unrelated error is reported too.
//
// Needs the SSR build for `sample-folder.js` (`npm run build --prefix designer/web`), like the studio smoke.

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, posix, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, "..", "..");
const WEB = resolve(REPO_ROOT, "designer", "web");
const MODULE = resolve(WEB, "dist-ssr", "sample-folder.js");

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

let samples;
try {
  samples = await import(pathToFileURL(MODULE).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: the built chrome is missing (${MODULE}). Build it first:\n` +
      "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
  );
  process.exit(2);
}

/** The text files under `dir` (repository-relative POSIX paths), as a picked folder hands them over. */
function readTree(dir) {
  const out = [];
  for (const name of readdirSync(resolve(REPO_ROOT, dir))) {
    const path = `${dir}/${name}`;
    if (statSync(resolve(REPO_ROOT, path)).isDirectory()) {
      out.push(...readTree(path));
    } else if (/\.(tsx|css|json)$/.test(name)) {
      out.push({ path, text: readFileSync(resolve(REPO_ROOT, path), "utf8") });
    }
  }
  return out;
}

const IMPORT =
  /(?:\b(?:import|export)\b[^;"']*?\bfrom\s*|\bimport\s*)["'](\.{1,2}\/[^"']*)["']/gs;

function importsOf(text) {
  const code = text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  return [...code.matchAll(IMPORT)].map((match) => match[1]);
}

// `generated/` holds a native build's output when a build tree sits inside `samples/`; a clone has none.
const tree = [...readTree("samples"), ...readTree("packages/tessera-ui")].filter(
  (file) => !file.path.includes("/generated/"),
);
const entries = samples.sampleEntries(tree);
// Absolute, because the compiler runs with `cwd` set to the assembled project: a repository-relative
// `--codegen build/...` (what the Pages lane passes) would not resolve from there.
const codegenArgument = argument("--codegen") ?? process.env.TESSERA_CODEGEN;
const codegen = codegenArgument ? resolve(REPO_ROOT, codegenArgument) : undefined;

await test("the repository root offers every sample directory that has a component", async () => {
  const withComponent = new Set(entries.map((entry) => entry.split("/")[1]));
  for (const name of readdirSync(resolve(REPO_ROOT, "samples"))) {
    const dir = resolve(REPO_ROOT, "samples", name);
    const tsx = statSync(dir).isDirectory() && readdirSync(dir).some((file) => file.endsWith(".tsx"));
    assert.equal(withComponent.has(name), tsx, `samples/${name}`);
  }
  assert.ok(entries.length > 0, "the repository has no sample entry");
});

for (const entry of entries) {
  const project = samples.sampleProject(tree, entry);
  const files = new Map(project.files.map((file) => [file.path, file.text]));

  await test(`${entry}: every relative import names a file of the assembled project`, async () => {
    assert.deepEqual(project.missing, [], samples.missingSentence(project.missing));
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

  await test(`${entry}: opened as samples/<name> alone, the advice names real library files`, async () => {
    const dir = posix.dirname(entry);
    const alone = tree
      .filter((file) => file.path.startsWith(`${dir}/`))
      .map((file) => ({ path: file.path.slice(dir.length + 1), text: file.text }));
    const opened = samples.sampleProject(alone, entry.slice(dir.length + 1));
    for (const item of opened.missing) {
      assert.ok(item.library !== null, `${item.importer}: ${item.specifier} leaves the sample's folder`);
      assert.ok(existsSync(resolve(REPO_ROOT, item.library)), `${item.library} is not in the repository`);
    }
  });

  if (codegen) {
    await test(`${entry}: the compiler reports no unresolved import`, async () => {
      const dir = mkdtempSync(join(tmpdir(), "tessera-sample-"));
      try {
        for (const [path, text] of files) {
          mkdirSync(dirname(join(dir, path)), { recursive: true });
          writeFileSync(join(dir, path), text);
        }
        // Relative POSIX paths from the project root: the CLI hashes ids from the path it is given, and a
        // Windows absolute path would not match `--root`.
        const inputs = [...files.keys()].filter((path) => path.endsWith(".tsx"));
        inputs.sort((left, right) => (left === entry ? -1 : right === entry ? 1 : 0));
        const run = spawnSync(codegen, ["--diagnostics-json", "diagnostics.json", "--root", ".", ...inputs], {
          encoding: "utf8",
          cwd: dir,
        });
        assert.ok(
          run.status === 0 || run.status === 1,
          `the compiler failed to run (${codegen}): ${run.error ? run.error.message : run.stderr}`,
        );
        const document = JSON.parse(readFileSync(join(dir, "diagnostics.json"), "utf8"));
        const messages = (document.diagnostics || []).map((item) => `${item.code}: ${item.message}`);
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
