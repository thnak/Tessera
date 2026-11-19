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

process.stdout.write(failures === 0 ? "\nproblems model: all passed\n" : `\nproblems model: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
