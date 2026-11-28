// SPDX-License-Identifier: MIT
//
// The Problems model's gate (owner decision, studio focus pass): the counts the status bar shows and the rows
// the Problems panel lists are derived from the diagnostics array's `severity`, in the compiler's order, with
// nothing sorted or dropped. The module is imported from `dist-ssr/problems.js` (the code the page ships;
// `npm run build` is a prerequisite and a missing build is exit code 2). The browser half is
// `workbench_problems.mjs`.
//
//   node scripts/ci/preview_problems.mjs

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const WEB = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "designer", "web");
const BUNDLE = resolve(WEB, "dist-ssr", "problems.js");

let problems;
try {
  problems = await import(pathToFileURL(BUNDLE).href);
} catch {
  process.stderr.write(`preview_problems: ${BUNDLE} is missing; run \`npm run build --prefix designer/web\` first\n`);
  process.exit(2);
}

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

const span = (file, line, column, offset, length = 3) => ({ file, line, column, offset, length });
const DIAGNOSTICS = [
  { severity: "error", code: "TESSERA_A", message: "first", span: span("ui/Home.tsx", 3, 5, 40) },
  { severity: "warning", code: "TESSERA_B", message: "second", span: span("ui/Home.tsx", 9, 1, 120) },
  { severity: "error", code: "TESSERA_C", message: "third", span: span("ui/Other.tsx", 1, 1, 0) },
  { severity: "info", code: "TESSERA_D", message: "fourth" },
  { severity: "warning", code: "TESSERA_E", message: "fifth", span: { file: "ui/Home.tsx" } },
];

await test("severity: the spec's two severities, plus info; anything else counts as an error", () => {
  assert.equal(problems.severityOf("error"), "error");
  assert.equal(problems.severityOf("warning"), "warning");
  assert.equal(problems.severityOf("info"), "info");
  assert.equal(problems.severityOf("note"), "info");
  assert.equal(problems.severityOf(undefined), "error");
  assert.equal(problems.severityOf("Fatal"), "error");
});

await test("snapshot: counts come from each entry's severity and the order is the compiler's", () => {
  const snapshot = problems.snapshotOf(DIAGNOSTICS);
  assert.equal(snapshot.ran, true);
  assert.deepEqual([snapshot.errors, snapshot.warnings, snapshot.infos], [2, 2, 1]);
  assert.deepEqual(snapshot.problems.map((row) => row.code), DIAGNOSTICS.map((entry) => entry.code));
  assert.equal(snapshot.problems.length, DIAGNOSTICS.length, "nothing is dropped");
  assert.deepEqual(problems.snapshotOf([]).problems, []);
  assert.equal(problems.snapshotOf([]).ran, true, "an empty array is a finished compile with no problems");
});

await test("rows: file, line, column and offset survive; a missing span is empty, never invented", () => {
  const rows = problems.snapshotOf(DIAGNOSTICS).problems;
  assert.deepEqual([rows[0].file, rows[0].line, rows[0].column, rows[0].offset, rows[0].length], ["ui/Home.tsx", 3, 5, 40, 3]);
  assert.equal(problems.locationOf(rows[0]), "ui/Home.tsx:3:5");
  assert.equal(rows[3].file, "");
  assert.equal(problems.locationOf(rows[3]), "");
  assert.equal(rows[3].offset, null);
  assert.equal(problems.locationOf(rows[4]), "ui/Home.tsx", "a file with no line is shown as the file");
  assert.equal(rows[4].offset, null, "no offset means the row jumps by line");
});

await test("filter: only the switched-on severities remain, in order", () => {
  const rows = problems.snapshotOf(DIAGNOSTICS).problems;
  assert.deepEqual(problems.filterProblems(rows, new Set(["error"])).map((row) => row.code), ["TESSERA_A", "TESSERA_C"]);
  assert.deepEqual(problems.filterProblems(rows, new Set(["warning", "info"])).map((row) => row.code), ["TESSERA_B", "TESSERA_D", "TESSERA_E"]);
  assert.deepEqual(problems.filterProblems(rows, new Set()), []);
  assert.equal(problems.filterProblems(rows, new Set(problems.SEVERITIES)).length, 5);
});

await test("summary: the screen reader sentence, before and after the first compile", () => {
  assert.equal(problems.summaryOf(problems.EMPTY_SNAPSHOT), "Problems: not run yet");
  assert.equal(problems.summaryOf(problems.snapshotOf([])), "Problems: 0 errors, 0 warnings");
  assert.equal(problems.summaryOf(problems.snapshotOf(DIAGNOSTICS.slice(0, 2))), "Problems: 1 error, 1 warning");
  assert.equal(problems.summaryOf(problems.snapshotOf(DIAGNOSTICS)), "Problems: 2 errors, 2 warnings, 1 info");
});

await test("store: publish notifies subscribers, an unsubscribed listener is not called, reset goes back to not run", () => {
  const store = problems.createProblemsStore();
  assert.equal(store.get().ran, false);
  let calls = 0;
  const off = store.subscribe(() => {
    calls += 1;
  });
  store.publish(DIAGNOSTICS);
  assert.equal(calls, 1);
  assert.equal(store.get().errors, 2);
  store.publish([]);
  assert.equal(calls, 2);
  assert.equal(store.get().problems.length, 0);
  off();
  store.publish(DIAGNOSTICS);
  assert.equal(calls, 2, "no call after unsubscribe");
  store.reset();
  assert.equal(store.get().ran, false);
});

await test("store: a hidden panel's cost is one assignment, with nobody subscribed no listener runs", () => {
  const store = problems.createProblemsStore();
  const before = store.get();
  store.publish(DIAGNOSTICS);
  assert.notEqual(store.get(), before);
  assert.equal(store.get().problems.length, 5);
});

await test("bare `number`: the compiler's TESSERA_JSX_TYPE_NUMBER entry becomes an error row with its span", () => {
  // The entry is the one `diagnose_source` emits for `useState<number>(0)` (scene_failure_tests.cpp pins the
  // native side byte for byte); the Studio's Problems panel and status bar must show it as a normal error.
  const entry = {
    severity: "error",
    code: "TESSERA_JSX_TYPE_NUMBER",
    message: "bare `number` is not a Tessera type",
    span: span("ui/Counter.tsx", 2, 38, 65, 6),
    suggestion: "write `int` or `float`",
  };
  const snapshot = problems.snapshotOf([entry]);
  assert.deepEqual([snapshot.errors, snapshot.warnings], [1, 0]);
  const [row] = snapshot.problems;
  assert.equal(row.code, "TESSERA_JSX_TYPE_NUMBER");
  assert.deepEqual([row.file, row.line, row.column, row.offset, row.length], ["ui/Counter.tsx", 2, 38, 65, 6]);
});

const row = (file, line, column, code, severity = "error", message = "m") => ({ key: 0, severity, code, message, file, line, column, offset: 0, length: 1 });

await test("grouping: files in path order, rows in position order, rows with no file last, counts per file", () => {
  const rows = [
    row("ui/b.tsx", 4, 1, "X"),
    row("", 0, 0, "NOFILE", "info"),
    row("ui/a.tsx", 9, 2, "Y", "warning"),
    row("ui/a.tsx", 2, 7, "Z"),
    row("ui/a.tsx", 2, 3, "Z"),
  ];
  const groups = problems.groupByFile(rows);
  assert.deepEqual(groups.map((group) => group.file), ["ui/a.tsx", "ui/b.tsx", ""]);
  assert.deepEqual(groups[0].problems.map((entry) => [entry.line, entry.column]), [[2, 3], [2, 7], [9, 2]]);
  assert.deepEqual([groups[0].errors, groups[0].warnings, groups[0].infos], [2, 1, 0]);
  assert.deepEqual([groups[2].errors, groups[2].infos], [0, 1]);
  assert.deepEqual(problems.groupByFile([]), []);
  assert.deepEqual(problems.groupByFile([...rows].reverse()).map((group) => group.file), ["ui/a.tsx", "ui/b.tsx", ""], "input order does not matter");
});

await test("sorting: same position is ordered by code then message, so the list never jumps between runs", () => {
  const a = row("f", 1, 1, "B", "error", "x");
  const b = row("f", 1, 1, "A", "error", "z");
  const c = row("f", 1, 1, "A", "error", "y");
  assert.deepEqual([a, b, c].sort(problems.compareProblems).map((entry) => entry.message), ["y", "z", "x"]);
});

await test("merge: a finding the compile and the project both name is listed once; compile-only findings stay", () => {
  const project = [row("ui/a.tsx", 2, 3, "TESSERA_JSX_X"), row("ui/b.tsx", 1, 1, "TESSERA_ANALYSIS_IMPORT_FILE")];
  const compile = [row("ui/a.tsx", 2, 5, "TESSERA_JSX_X"), row("ui/a.tsx", 8, 1, "TESSERA_IR_LOWER")];
  const merged = problems.mergeProblems(compile, project);
  assert.deepEqual(merged.map((entry) => entry.code), ["TESSERA_JSX_X", "TESSERA_ANALYSIS_IMPORT_FILE", "TESSERA_IR_LOWER"]);
  assert.deepEqual(merged.map((entry) => entry.key), [0, 1, 2], "keys are unique after a merge");
  assert.deepEqual(problems.mergeProblems(compile, null).map((entry) => entry.code), ["TESSERA_JSX_X", "TESSERA_IR_LOWER"]);
});

await test("store: project rows reach the counts and a republish replaces them (fixing a file clears its row)", () => {
  const store = problems.createProblemsStore();
  let calls = 0;
  store.subscribe(() => {
    calls += 1;
  });
  store.publishProject([row("ui/Other.tsx", 1, 1, "TESSERA_JSX_X"), row("ui/Other.tsx", 3, 1, "TESSERA_W", "warning")]);
  assert.equal(store.get().ran, true, "the project rows alone are a finished run");
  assert.deepEqual([store.get().errors, store.get().warnings], [1, 1]);
  store.publish([{ severity: "error", code: "TESSERA_IR_LOWER", message: "m", span: span("ui/Home.tsx", 2, 2, 5) }]);
  assert.equal(store.get().errors, 2, "compile rows add to the project rows");
  store.publishProject([row("ui/Other.tsx", 3, 1, "TESSERA_W", "warning")]);
  assert.deepEqual([store.get().errors, store.get().warnings], [1, 1]);
  assert.equal(store.get().problems.some((entry) => entry.code === "TESSERA_JSX_X"), false);
  store.publishProject(null);
  assert.equal(store.get().problems.length, 1, "dropping the project rows leaves the compile rows");
  assert.equal(calls, 4);
  store.reset();
  assert.equal(store.get().ran, false);
  assert.equal(store.get().problems.length, 0, "a closed project clears both sources");
});

process.stdout.write(failures === 0 ? "\nproblems model: all passed\n" : `\nproblems model: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
