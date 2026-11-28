// SPDX-License-Identifier: MIT
//
// The project-wide Problems gate (#575, #569, ADR-060): the page's own analysis and compile clients
// (dist-ssr/analysis-client.js, compile-client.js, project-problems.js, problems.js: the code the page ships)
// driven against the real worker sources under Node, with stand-in engines for the Wasm modules.
//
//   node scripts/ci/preview_project_problems.mjs     (needs \`npm run build:chrome --prefix designer/web\`; exit 2 without it)
//
// What it proves: an error in a file that is not open reaches the store with its file, line and a UTF-16 offset
// the editor can reveal; fixing the file clears the row; rows group by file in path order; an unavailable
// analysis engine leaves the compile rows alone and is not retried; the compile client restarts a killed
// worker from the next snapshot without raising an error to the page.

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { analysisServiceSource } from "../../designer/web/services/analysis-service.js";
import { compileServiceSource } from "../../designer/web/services/compile-service.js";
import { nodeWorker } from "./mock_test_support.mjs";

const DIST = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "designer", "web", "dist-ssr");
const load = async (name) => {
  try {
    return await import(pathToFileURL(resolve(DIST, name)).href);
  } catch {
    process.stderr.write(`preview_project_problems: ${name} is missing under ${DIST}; run \u0060npm run build:chrome --prefix designer/web\u0060 first\n`);
    process.exit(2);
  }
};
const problems = await load("problems.js");
const projectProblems = await load("project-problems.js");
const { createAnalysisClient } = await load("analysis-client.js");
const { createCompileClient } = await load("compile-client.js");

let failures = 0;
async function test(name, fn) {
  try {
    await fn();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${String(error.message).split("\n").join("\n     ")}\n`);
  }
}

// One diagnostic per "@@error" in a file, with the UTF-16 span of the marker.
const ANALYSIS_ENGINE = String.raw`
let project = new Map();
function tesseraAnalysisSnapshot(files) { project = new Map(files.map((f) => [f.file, f.text])); }
function tesseraAnalysisEdits() { return true; }
function tesseraAnalysisQuery(type, file) {
  const files = [...project.keys()].sort((a, b) => a - b).filter((id) => file < 0 || id === file);
  return JSON.stringify({ files: files.map((id) => {
    const text = project.get(id);
    const at = text.indexOf('@@error');
    return { file: id, diagnostics: at < 0 ? [] : [{ code: 'TESSERA_JSX_STANDIN', severity: 'error', message: 'broken', from: at, to: at + 7 }] };
  }) });
}`;
const MISSING_ENGINE = 'async function tesseraAnalysisSnapshot() { throw new Error("module missing"); } function tesseraAnalysisEdits() { return true; } function tesseraAnalysisQuery() { return "{}"; }';
const COMPILE_ENGINE = String.raw`
function tesseraCompile(project) {
  if (project.sources.some((s) => s.text.includes('@@die'))) setTimeout(() => { throw new Error('killed'); }, 0);
  const diagnostics = project.sources.filter((s) => s.text.includes('@@error')).map((s) => ({ severity: 'error', code: 'C', message: 'm', span: { file: s.path, line: 1, column: 1, offset: 0, length: 1 } }));
  return JSON.stringify({ diagnostics });
}`;

const emoji = "\u{1F600}";
const files = [
  { path: "ui/App.tsx", kind: "source" },
  { path: "ui/Card.tsx", kind: "source" },
  { path: "ui/theme.css", kind: "stylesheet" },
  { path: "tessera.json", kind: "config" },
];
const texts = new Map([
  ["ui/App.tsx", "export function App() { return <Card />; }\n"],
  ["ui/Card.tsx", `// ${emoji}\nline two @@error here\n`],
  ["ui/theme.css", ".a {}\n"],
  ["tessera.json", "{}"],
]);
const textOf = (path) => texts.get(path) ?? "";

await test("entries: sources and stylesheets only, with project paths and stable ids", () => {
  const entries = projectProblems.analysisEntries(files, textOf);
  assert.deepEqual(entries.map((entry) => [entry[0], entry[2]]), [[0, "ui/App.tsx"], [1, "ui/Card.tsx"], [2, "ui/theme.css"]]);
});

await test("line and column: 1-based in UTF-16 units, an offset past the end clamps", () => {
  assert.deepEqual(projectProblems.lineColumnOf("ab\ncd\n", 0), { line: 1, column: 1 });
  assert.deepEqual(projectProblems.lineColumnOf("ab\ncd\n", 4), { line: 2, column: 2 });
  assert.deepEqual(projectProblems.lineColumnOf("ab\ncd\n", 3), { line: 2, column: 1 });
  assert.deepEqual(projectProblems.lineColumnOf(`${emoji}x`, 2), { line: 1, column: 3 });
  assert.deepEqual(projectProblems.lineColumnOf("ab", 99), { line: 1, column: 3 });
});

await test("an error in a file that is not open reaches the store; fixing it clears the row", async () => {
  const client = createAnalysisClient(() => nodeWorker(analysisServiceSource(ANALYSIS_ENGINE)));
  const store = problems.createProblemsStore();
  store.publishProject(await client.problems(files, textOf));
  const [row] = store.get().problems;
  assert.equal(store.get().errors, 1);
  assert.deepEqual([row.file, row.line, row.column], ["ui/Card.tsx", 2, 10]);
  const text = textOf("ui/Card.tsx");
  assert.equal(text.slice(row.offset, row.offset + row.length), "@@error", "the offset is what the editor reveals");
  texts.set("ui/Card.tsx", "export function Card() { return <p />; }\n");
  store.publishProject(await client.problems(files, textOf));
  assert.equal(store.get().problems.length, 0);
  assert.equal(store.get().ran, true);
  texts.set("ui/Card.tsx", `// ${emoji}\nline two @@error here\n`);
  client.dispose();
});

await test("the panel's model: rows group by file in path order across the project", async () => {
  texts.set("ui/App.tsx", "@@error\n");
  const client = createAnalysisClient(() => nodeWorker(analysisServiceSource(ANALYSIS_ENGINE)));
  const rows = await client.problems(files, textOf);
  assert.deepEqual(problems.groupByFile(rows).map((group) => group.file), ["ui/App.tsx", "ui/Card.tsx"]);
  texts.set("ui/App.tsx", "export function App() { return <Card />; }\n");
  client.dispose();
});

await test("an unavailable analysis module is not an error: no rows, and it is not asked again", async () => {
  let created = 0;
  const client = createAnalysisClient(() => {
    created += 1;
    return nodeWorker(analysisServiceSource(MISSING_ENGINE));
  });
  assert.equal(await client.problems(files, textOf), null);
  assert.equal(await client.problems(files, textOf), null);
  assert.equal(created, 1, "one worker, one failed snapshot, then the client stays quiet");
  client.dispose();
});

await test("the compile client: a killed worker is restarted from the next snapshot with no error for the page", async () => {
  let created = 0;
  const client = createCompileClient(() => {
    created += 1;
    return nodeWorker(compileServiceSource(COMPILE_ENGINE));
  });
  const project = (text) => ({ entry: "ui/App.tsx", sources: [{ path: "ui/App.tsx", text }], stylesheets: [] });
  assert.equal((await client.diagnostics(project("fine"))).diagnostics.length, 0);
  await client.diagnostics(project("@@die")); // answers, then the worker's timer throws and it dies
  await new Promise((done) => setTimeout(done, 100));
  const after = await client.diagnostics(project("@@error"));
  assert.equal(after.diagnostics.length, 1, "answered by the restarted worker");
  assert.equal(created, 2, "exactly one restart");
  client.dispose();
});

process.stdout.write(failures === 0 ? "\nproject problems: all passed\n" : `\nproject problems: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
