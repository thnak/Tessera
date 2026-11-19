// SPDX-License-Identifier: MIT
//
// The studio's path-confinement rule (issue #391, D-S4 of docs/specs/designer-studio-v1.md section 7,
// and its section 10 "Security").
//
//   node scripts/ci/preview_project_paths.mjs
//
// **The rule, stated as a rule.** Every project-relative path the studio handles -- a zip entry name, a
// `FileSystemHandle` name joined to its parent, a path a shell constructed, a path a save will write --
// must name a file *inside* the opened project folder. `confinePath` is that rule, and it is the only
// place it is decided, so this test is the proof that the rule holds rather than an illustration of one
// code path:
//
//   1. **It is total.** Every input either returns a canonical path or throws a `PathError` with a
//      named code. There is no third outcome and no input that escapes as a crash.
//   2. **It is closed.** A canonical path never starts with `/`, never contains `..`, never contains a
//      backslash, and never names a reserved device. That is stated as a property over the whole
//      accepted corpus, not one case at a time, because it is the invariant a caller leans on.
//   3. **It refuses every escape the spec names** -- absolute, drive letter, `..`, UNC, NUL -- each with
//      the code that says *which* escape it was, so a shell's message can be specific.
//   4. **It refuses what a filesystem would silently reinterpret.** A Windows device name (`CON`,
//      `NUL`, `COM1.tsx`) is not a file; a name with a trailing dot or space is normalized by Win32
//      before the call; a path with a control character is not a path.
//   5. **It agrees with the compiler.** `joinProjectPath` mirrors the C++ `join_project_path` that
//      resolves imports, so the file the studio thinks an import names is the file the compiler reads.
//      A disagreement here would be a preview of a different file than the build compiles.
//   6. **It classifies the accepted set of the spec's section 7** and locks everything else -- C++,
//      generated output, `node_modules`, a stray binary -- so the lock badge and the "never read, never
//      sent to the viewer" sentence come from the same decision.
//
// **No browser, no DOM, no page.** This module and `studio-zip.js` are the two framework-independent
// pieces of the issue: no imports, no `window`, so the React shell (#422) imports them unchanged. The
// file tree, the editor and the multi-file page wiring are deferred to that shell, which is why this
// file tests nothing about them.
//
// Browser verification: **not performed.** The folder picker, Save, and the drop handler need the File
// System Access API and a page; #391 is blocked on #422's shell.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import {
  FILE_KIND,
  LIMITS,
  PathError,
  classifyPath,
  confinePath,
  extensionOf,
  isConfined,
  isTextKind,
  isUnder,
  joinProjectPath,
  limitMessage,
  parentOf,
  baseName,
} from '../../designer/web/project-paths.js';

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

// The escapes the spec's section 10 names, plus the ones a filesystem reinterprets. Each is
// `[input, code]`: the code is what a shell's message names, so it is part of the contract.
const REFUSED = [
  ['/etc/passwd', 'absolute'],
  ['/', 'absolute'],
  ['C:/Windows/System32/drivers/etc/hosts', 'absolute'],
  ['c:relative.tsx', 'absolute'],
  ['../outside.tsx', 'traversal'],
  ['ui/../../outside.tsx', 'traversal'],
  ['ui/..', 'traversal'],
  ['..', 'traversal'],
  ['ui\\App.tsx', 'backslash'],
  ['\\\\server\\share\\x.tsx', 'backslash'],
  ['', 'empty'],
  ['./', 'empty'],
  ['.', 'empty'],
  ['ui/\u0000App.tsx', 'control'],
  ['ui/\u001b[31mred', 'control'],
  ['NUL', 'reserved'],
  ['nul', 'reserved'],
  ['con', 'reserved'],
  ['CON', 'reserved'],
  ['ui/COM1.tsx', 'reserved'],
  ['ui/aux.css', 'reserved'],
  ['ui/LPT9', 'reserved'],
];

for (const [path, code] of REFUSED) {
  test(`refuses ${JSON.stringify(path)} as ${code}`, () => {
    assert.throws(
      () => confinePath(path),
      (error) => {
        assert.ok(error instanceof PathError, `expected a PathError, got ${error}`);
        assert.equal(error.code, code, `code for ${JSON.stringify(path)}`);
        // The message names the path, so a panel can show it rather than "invalid path".
        assert.ok(error.message.includes('path'), 'the message is about a path');
        return true;
      },
    );
    assert.equal(isConfined(path), false, 'the predicate agrees');
  });
}

// A path the rule accepts, and the canonical form it produces. `./` and a doubled or single-dot
// segment are normalized, because a zip and a handle both produce them and neither is an escape.
const ACCEPTED = [
  ['tessera.json', 'tessera.json'],
  ['ui/App.tsx', 'ui/App.tsx'],
  ['./ui/App.tsx', 'ui/App.tsx'],
  ['ui//App.tsx', 'ui/App.tsx'],
  ['ui/./App.tsx', 'ui/App.tsx'],
  ['././ui/App.tsx', 'ui/App.tsx'],
  ['ui/deep/naïve page.tsx', 'ui/deep/naïve page.tsx'],
  ['styles/theme.css', 'styles/theme.css'],
  ['a/b/c/d/e.tsx', 'a/b/c/d/e.tsx'],
  ['UI/App.TSX', 'UI/App.TSX'],
];

for (const [path, canonical] of ACCEPTED) {
  test(`accepts ${JSON.stringify(path)} as ${JSON.stringify(canonical)}`, () => {
    assert.equal(confinePath(path), canonical);
    assert.equal(isConfined(path), true);
  });
}

test('the rule is total: every input is a path or a named refusal', () => {
  // The property, not one case: no input reaches a crash, a `null`, or a path that is not canonical.
  const inputs = [...REFUSED.map(([path]) => path), ...ACCEPTED.map(([path]) => path)];
  for (const input of inputs) {
    let outcome;
    try {
      outcome = { kind: 'path', value: confinePath(input) };
    } catch (error) {
      outcome = { kind: 'error', value: error };
    }
    assert.ok(
      outcome.kind === 'path' || (outcome.value instanceof PathError),
      `${JSON.stringify(input)} produced neither a path nor a PathError`,
    );
    if (outcome.kind === 'path') {
      assert.equal(typeof outcome.value, 'string');
      assert.ok(outcome.value.length > 0, 'an accepted path is non-empty');
    }
  }
});

test('the rule is closed: a canonical path cannot escape the root', () => {
  // The invariant stated over the whole accepted corpus. A caller may concatenate a canonical path with
  // anything and still be inside the project, which is what makes "never construct a path outside the
  // opened folder" true by construction rather than by review.
  for (const [path] of ACCEPTED) {
    const canonical = confinePath(path);
    assert.ok(!canonical.startsWith('/'), `${path}: not absolute`);
    assert.ok(!canonical.includes('\\'), `${path}: POSIX separators only`);
    assert.ok(!canonical.split('/').includes('..'), `${path}: no dot-dot segment`);
    assert.ok(!canonical.split('/').includes('.'), `${path}: no dot segment`);
    assert.ok(!canonical.split('/').includes(''), `${path}: no empty segment`);
    assert.equal(confinePath(canonical), canonical, `${path}: canonicalization is idempotent`);
  }
});

test('a path longer than the limit is refused, not truncated', () => {
  const long = 'a'.repeat(LIMITS.maxPathBytes) + '.tsx';
  assert.throws(
    () => confinePath(long),
    (error) => error instanceof PathError && error.code === 'too-long',
  );
  // Exactly at the limit is accepted: the boundary is inclusive, because a file named at the limit is a
  // file a person may have.
  const exact = 'a'.repeat(LIMITS.maxPathBytes - 4) + '.tsx';
  assert.equal(confinePath(exact).length, LIMITS.maxPathBytes);
});

test('parentOf, baseName and extensionOf agree with the canonical form', () => {
  assert.equal(parentOf('ui/App.tsx'), 'ui');
  assert.equal(parentOf('tessera.json'), '');
  assert.equal(parentOf('a/b/c.css'), 'a/b');
  assert.equal(baseName('ui/App.tsx'), 'App.tsx');
  assert.equal(baseName('tessera.json'), 'tessera.json');
  assert.equal(extensionOf('ui/App.tsx'), '.tsx');
  assert.equal(extensionOf('ui/App.TSX'), '.tsx');
  assert.equal(extensionOf('Makefile'), '');
  assert.equal(extensionOf('.gitignore'), '');
  assert.equal(isUnder('ui/deep/x.tsx', 'ui'), true);
  assert.equal(isUnder('ui', 'ui'), true);
  assert.equal(isUnder('ui2/x.tsx', 'ui'), false);
  // The root: every path is under it.
  assert.equal(isUnder('anything/at/all', ''), true);
});

test('joinProjectPath mirrors the compiler’s join_project_path', () => {
  // Cases from src/compiler/lowering/src/project_path.cpp. The studio and the compiler must name the
  // same file for an import or a preview shows a different file than the build compiles.
  assert.equal(joinProjectPath('ui', './Home'), 'ui/Home');
  assert.equal(joinProjectPath('ui/deep', '../Home'), 'ui/Home');
  assert.equal(joinProjectPath('', './Home'), 'Home');
  assert.equal(joinProjectPath('ui', '../Home'), 'Home');
  // A `..` with nothing to pop is dropped, exactly as the C++ function does (it pops only when the
  // stack is non-empty). Being *stricter* here would disagree with the compiler about which file an
  // import names; the confinement of the result is what keeps it inside the project.
  assert.equal(joinProjectPath('../..', 'x'), 'x');
  assert.equal(joinProjectPath('', '../../x'), 'x');
  assert.equal(joinProjectPath('ui', 'Row'), 'ui/Row');
  // The result is always confined, so a specifier that climbs above the root cannot address outside it.
  for (const [base, specifier] of [
    ['ui', './x.css'],
    ['ui', '../shared/y.css'],
    ['', '../../z.tsx'],
    ['a/b/c', '../../../d.tsx'],
  ]) {
    assert.equal(confinePath(joinProjectPath(base, specifier)), joinProjectPath(base, specifier));
  }
});

test('classifyPath accepts the spec’s set and locks everything else', () => {
  const context = { themePath: 'assets/theme.json', assetPaths: ['assets/fonts', 'assets/images'] };
  // Accepted: the five kinds of spec section 7.
  assert.equal(classifyPath('tessera.json', context), FILE_KIND.config);
  assert.equal(classifyPath('ui/App.tsx', context), FILE_KIND.source);
  assert.equal(classifyPath('ui/App.jsx', context), FILE_KIND.source);
  assert.equal(classifyPath('ui/deep/Page.tsx', context), FILE_KIND.source);
  assert.equal(classifyPath('styles/theme.css', context), FILE_KIND.stylesheet);
  assert.equal(classifyPath('assets/theme.json', context), FILE_KIND.theme);
  assert.equal(classifyPath('ui/theme.json', context), FILE_KIND.theme);
  assert.equal(classifyPath('assets/fonts/Inter.ttf', context), FILE_KIND.asset);
  assert.equal(classifyPath('assets/images/logo.png', context), FILE_KIND.asset);
  assert.equal(classifyPath('assets/images/nested/dot.webp', context), FILE_KIND.asset);
  // Locked: what ADR-040 and the spec say must never be edited or sent to the viewer. A text file opens
  // view-only (owner decision 2026-11-17); a binary or build output stays name-and-size only.
  assert.equal(classifyPath('ui/App.g.hpp', context), FILE_KIND.viewOnly);
  assert.equal(classifyPath('ui/App.cpp', context), FILE_KIND.viewOnly);
  assert.equal(classifyPath('gen/AppBase.hpp', context), FILE_KIND.viewOnly);
  assert.equal(classifyPath('package-lock.json', context), FILE_KIND.viewOnly);
  assert.equal(classifyPath('CMakeLists.txt', context), FILE_KIND.viewOnly);
  assert.equal(classifyPath('node_modules/react/index.js', context), FILE_KIND.other);
  assert.equal(classifyPath('build/out.txt', context), FILE_KIND.other);
  assert.equal(classifyPath('app.exe', context), FILE_KIND.other);
});

test('only a text kind is read, edited and sent to the viewer', () => {
  for (const kind of [FILE_KIND.source, FILE_KIND.stylesheet, FILE_KIND.config, FILE_KIND.theme]) {
    assert.equal(isTextKind(kind), true, `${kind} is text`);
  }
  // An asset and anything else are listed with a size and never opened.
  for (const kind of [FILE_KIND.asset, FILE_KIND.other, FILE_KIND.viewOnly]) {
    assert.equal(isTextKind(kind), false, `${kind} is locked`);
  }
});

test('the limits are the real ones, not ui.limits.*', () => {
  // The per-file cap is the compiler's own TESSERA_JSX_LIMIT_FILE_SIZE, so a file this module accepts
  // is a file the front end will read rather than refuse one stage later (issue #405 records that
  // `ui.limits.*` is documented but unread).
  assert.equal(LIMITS.maxFileBytes, 1048576, '1 MiB: TESSERA_JSX_LIMIT_FILE_SIZE');
  // The owner's decision of 2026-11-16 in issue #391.
  assert.equal(LIMITS.maxAcceptedFiles, 500);
  assert.equal(LIMITS.maxAcceptedBytes, 20 * 1024 * 1024);
  // Every limit has a sentence that names it, so a refusal is never "a limit was exceeded".
  for (const key of ['maxAcceptedFiles', 'maxAcceptedBytes', 'maxTreeEntries', 'maxArchiveBytes', 'maxFileBytes']) {
    assert.ok(key in LIMITS, `${key} is a limit`);
    const message = limitMessage(key, 1);
    assert.ok(message.length > 10, `${key} has a sentence`);
    assert.ok(!message.includes('undefined'), `${key} has no undefined in its sentence`);
  }
});

test('the module has no imports and no DOM dependency', () => {
  // The framework-independence claim, asserted rather than described: a React shell (#422) imports this
  // file unchanged, so it must not reach for a page. Checked by reading the module's own source, which
  // is the one place the claim can be verified without a browser.
  const source = readSource();
  assert.ok(!/^import\s/m.test(source), 'no import statement');
  assert.ok(!/\bdocument\s*\./.test(source), 'no document');
  assert.ok(!/\bwindow\s*\./.test(source), 'no window');
  assert.ok(!/localStorage|navigator|fetch\(/.test(source), 'no storage, navigator or network');
});

function readSource() {
  return readFileSync(
    fileURLToPath(new URL('../../designer/web/project-paths.js', import.meta.url)),
    'utf8',
  );
}

process.stdout.write(
  failures === 0 ? '\nproject paths: all cases passed\n' : `\nproject paths: ${failures} failed\n`,
);
if (failures > 0) {
  process.exitCode = 1;
}
