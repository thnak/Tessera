// SPDX-License-Identifier: MIT
//
// "May be out of date" advice for generated files, tested without a browser (issue #391, owner decision
// 2026-11-17, docs/specs/designer-studio-v1.md section 7).
//
//   node scripts/ci/preview_generated_staleness.mjs
//
// The rule is a heuristic and the cases below pin both its flags and its silences: a generated header is
// flagged only when its source resolves by the project's own naming and is strictly newer, and it stays
// silent whenever the source is missing, ambiguous or the time is unknown (a zip carries none).
import assert from 'node:assert/strict';

import { generatedComponent, sourceOfComponent, staleGenerated } from '../../designer/web/generated-staleness.js';

let failures = 0;
function test(name, body) {
  try {
    body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

const source = (path, text, modified) => ({ path, kind: 'source', text, modified });
const generated = (path, modified) => ({ path, kind: 'view-only', text: '', modified });

test('only <Component>.g.hpp / .g.h names a component', () => {
  assert.equal(generatedComponent('gen/Home.g.hpp'), 'Home');
  assert.equal(generatedComponent('Home.g.h'), 'Home');
  assert.equal(generatedComponent('src/Home.hpp'), '');
  assert.equal(generatedComponent('src/Home.cpp'), '');
  assert.equal(generatedComponent('gen/9bad.g.hpp'), '');
});

test('the source is the file named for the component, else the one that declares it', () => {
  const home = source('ui/Home.tsx', 'export function Home() {}', 1);
  assert.equal(sourceOfComponent('Home', [home])?.path, 'ui/Home.tsx');
  const screens = source('ui/screens.tsx', 'export function Home() {}\nexport function Other() {}', 1);
  assert.equal(sourceOfComponent('Home', [screens])?.path, 'ui/screens.tsx');
  assert.equal(sourceOfComponent('Other', [screens])?.path, 'ui/screens.tsx');
  assert.equal(sourceOfComponent('Missing', [screens]), null);
  // Two candidates: no guess, so no badge.
  assert.equal(sourceOfComponent('Home', [home, source('other/Home.tsx', '', 1)]), null);
  assert.equal(sourceOfComponent('Home', [source('a.tsx', 'function Home(){}', 1), source('b.tsx', 'const Home = 1', 1)]), null);
});

test('a generated file older than its source is flagged; an equal or newer one is not', () => {
  const home = source('ui/Home.tsx', 'export function Home() {}', 200);
  assert.deepEqual(staleGenerated([home, generated('gen/Home.g.hpp', 100)]), ['gen/Home.g.hpp']);
  assert.deepEqual(staleGenerated([home, generated('gen/Home.g.hpp', 200)]), []);
  assert.deepEqual(staleGenerated([home, generated('gen/Home.g.hpp', 300)]), []);
});

test('it stays silent without a source, without times, or for a file that is not generated', () => {
  const home = source('ui/Home.tsx', 'export function Home() {}', 200);
  assert.deepEqual(staleGenerated([generated('gen/Orphan.g.hpp', 100)]), [], 'no source: no badge');
  assert.deepEqual(staleGenerated([source('ui/Home.tsx', '', 0), generated('gen/Home.g.hpp', 100)]), [], 'a zip has no times');
  assert.deepEqual(staleGenerated([home, generated('gen/Home.g.hpp', 0)]), []);
  assert.deepEqual(staleGenerated([home, generated('src/Home.cpp', 100)]), [], 'hand-written C++ is not generated');
});

test('the result is sorted', () => {
  const home = source('ui/Home.tsx', 'export function Home() {}', 200);
  const about = source('ui/About.tsx', 'export function About() {}', 200);
  assert.deepEqual(
    staleGenerated([generated('gen/Home.g.hpp', 1), home, generated('gen/About.g.hpp', 1), about]),
    ['gen/About.g.hpp', 'gen/Home.g.hpp'],
  );
});

if (failures > 0) {
  process.stdout.write(`\ngenerated staleness: ${failures} case(s) failed\n`);
  process.exit(1);
}
process.stdout.write('\ngenerated staleness: all cases passed\n');
