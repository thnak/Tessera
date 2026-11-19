// SPDX-License-Identifier: MIT
//
// The page-side style override client (issue #585, designer-protocol-v1 sections 9 and 10) against a fake
// canvas module, and the Undo/Redo routing table (rule D3).
//
//   node scripts/ci/preview_override_client.mjs
//
// The fake module is a small JavaScript model of what `tools/codegen/wasm_canvas_override.cpp` and
// `tessera_canvas_patch` answer (the engine half is `scene_canvas_override*_tests`); the page code under test
// is the real `callExport` marshalling, the real `createOverrideClient`, the real routing functions and the
// real command table. Needs Node with type stripping (v22.18+).

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createOverrideClient } from '../../designer/web/src/lib/override-client.ts';
import { applyChanges, EditRangeError, toTextChanges } from '../../designer/web/src/lib/utf8-edits.ts';
import { menuState, runEdit, targetFor } from '../../designer/web/src/lib/edit-routing.ts';
import { hexOf, isColorProperty } from '../../designer/web/src/lib/color-hex.ts';
import { callExport } from '../../designer/web/src/lib/wasm-call.ts';
import { buildItems, createContext, createRegistry } from '../../designer/web/src/workbench/index.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const COMMANDS = JSON.parse(readFileSync(resolve(ROOT, 'designer/web/src/workbench/commands.json'), 'utf8')).commands;

let failures = 0;
async function check(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n${error.stack}\n`);
  }
}

// ---- the fake canvas module -------------------------------------------------------------------------

const KNOWN = new Set(['color', 'background', 'padding-left', 'margin-left']);
const APP = 'ui/App.tsx';
// A non-ASCII character before the edit makes the byte offsets differ from the UTF-16 indices.
const APP_TEXT = '// héllo 𝄞\nexport function App() { return <View>x</View>; }\n';
const THEME = '{"themes":{"light":{"--accent":"#3366ff"}},"start":"light"}';

/**
 * `settleWith` decides what the next evaluation gives the node: 'match' consumes the overrides, anything
 * else is a mismatch. `module.calls` records the exports in order.
 */
function fakeModule(options = {}) {
  const heap = new Map();
  let next = 1;
  const calls = [];
  const state = {
    entries: [], // {node, property, value}
    undo: [],
    redo: [],
    theme: null,
    settleWith: 'match',
    patchBase: { [APP]: 1 },
  };
  const snapshot = () => state.entries.map((entry) => ({ ...entry }));
  const doc = (value) => {
    const pointer = 5000 + next++;
    heap.set(pointer, JSON.stringify(value));
    return pointer;
  };
  const report = (extra) => ({
    overrideVersion: 1,
    ok: true,
    changed: false,
    canUndo: state.undo.length > 0,
    canRedo: state.redo.length > 0,
    overrides: state.entries,
    ...extra,
  });
  const forNode = (node) => state.entries.filter((entry) => entry.node === node);

  const module = {
    calls,
    state,
    _malloc: () => next++,
    _free: (pointer) => heap.delete(pointer),
    _tessera_free: () => {},
    lengthBytesUTF8: (text) => Buffer.byteLength(text),
    stringToUTF8: (text, pointer) => heap.set(pointer, text),
    UTF8ToString: (pointer) => heap.get(pointer),
    _tessera_canvas_override(opP, nodeP, propP, valueP) {
      const [op, node, property, value] = [opP, nodeP, propP, valueP].map((pointer) => heap.get(pointer));
      calls.push(['override', op, node, property, value]);
      const push = () => {
        state.undo.push(snapshot());
        state.redo = [];
      };
      if (op === 'set') {
        if (!KNOWN.has(property)) {
          return doc(
            report({
              ok: false,
              reason: 'invalid_override',
              diagnostics: [{ code: 'TESSERA_CSS_UNKNOWN_PROPERTY', message: `unknown property \`${property}\`` }],
              overrides: forNode(node),
            }),
          );
        }
        const existing = state.entries.find((entry) => entry.node === node && entry.property === property);
        if (existing?.value === value) {
          return doc(report({ overrides: forNode(node) }));
        }
        push();
        if (existing) {
          existing.value = value;
        } else {
          state.entries.push({ node, property, value });
        }
        return doc(report({ changed: true, overrides: forNode(node) }));
      }
      if (op === 'clear') {
        const matches = (entry) => entry.node === node && (property === '' || entry.property === property);
        if (!state.entries.some(matches)) {
          return doc(report({ overrides: forNode(node) }));
        }
        push();
        state.entries = state.entries.filter((entry) => !matches(entry));
        return doc(report({ changed: true, overrides: forNode(node) }));
      }
      if (op === 'revert') {
        if (state.entries.length === 0) {
          return doc(report());
        }
        push();
        state.entries = [];
        return doc(report({ changed: true }));
      }
      if (op === 'undo' || op === 'redo') {
        const [from, to] = op === 'undo' ? [state.undo, state.redo] : [state.redo, state.undo];
        if (from.length === 0) {
          return doc(report());
        }
        to.push(snapshot());
        state.entries = from.pop();
        return doc(report({ changed: true }));
      }
      return doc(report({ ok: false, reason: 'unknown_operation' }));
    },
    _tessera_canvas_set_theme(fileP, textP, idP) {
      calls.push(['set_theme', heap.get(fileP), heap.get(textP), heap.get(idP)]);
      state.theme = heap.get(fileP) === '' ? null : { file: heap.get(fileP), text: heap.get(textP), id: heap.get(idP) };
      return doc({ ok: true });
    },
    _tessera_canvas_patch(nodeP, destP, classP, fileP, tokenP, valueP) {
      const [node, destination, className, file, token, tokenValue] = [nodeP, destP, classP, fileP, tokenP, valueP].map(
        (pointer) => heap.get(pointer),
      );
      calls.push(['patch', node, destination, className, file, token, tokenValue]);
      if (forNode(node).length === 0) {
        return doc({ patchVersion: 1, ok: false, reason: 'no_overrides', detail: 'the node has no overrides to commit' });
      }
      if (destination === 'token') {
        if (state.theme === null) {
          return doc({ patchVersion: 1, ok: false, reason: 'not_token_bound', detail: 'no theme asset' });
        }
        const at = Buffer.from(state.theme.text).indexOf('"#3366ff"');
        return doc({
          patchVersion: 1,
          ok: true,
          patchId: 'p1',
          edits: [{ file: state.theme.file, offset: at, length: 9, newText: JSON.stringify(tokenValue) }],
          baseVersions: { [state.theme.file]: 1 },
        });
      }
      const at = Buffer.from(APP_TEXT).indexOf('<View>') + '<View'.length;
      const declarations = forNode(node)
        .map((entry) => `${entry.property}: "${entry.value}"`)
        .join(', ');
      return doc({
        patchVersion: 1,
        ok: true,
        patchId: 'p1',
        edits: [{ file: APP, offset: at, length: 0, newText: ` style={{ ${declarations} }}` }],
        baseVersions: state.patchBase,
      });
    },
    _tessera_canvas_settle(nodeP) {
      const node = heap.get(nodeP);
      calls.push(['settle', node]);
      if (state.settleWith === 'match') {
        // The engine's consume: the node's entries leave the layer, and its steps leave the history.
        const strip = (snap) => snap.filter((entry) => entry.node !== node);
        state.entries = strip(state.entries);
        const collapse = (stack, live) => {
          const kept = [];
          stack.forEach((snap, index) => {
            const after = index + 1 < stack.length ? stack[index + 1] : live;
            if (JSON.stringify(snap) !== JSON.stringify(after)) {
              kept.push(snap);
            }
          });
          return kept;
        };
        state.undo = collapse(state.undo.map(strip), state.entries);
        state.redo = collapse([...state.redo].reverse().map(strip), state.entries).reverse();
        const result = { settleVersion: 1, ok: true, consumed: true, overrides: [] };
        if (!options.oldSettle) {
          result.canUndo = state.undo.length > 0;
          result.canRedo = state.redo.length > 0;
        }
        return doc(result);
      }
      return doc({
        settleVersion: 1,
        ok: false,
        reason: 'patch_mismatch',
        consumed: false,
        diagnostics: [{ code: 'TESSERA_DESIGNER_PATCH_MISMATCH', message: 'the committed source does not give the node the overridden style' }],
        overrides: forNode(node),
      });
    },
  };
  return module;
}

function clientFor(module) {
  return createOverrideClient({ invoke: (name, strings) => callExport(module, name, strings) });
}

/** A fake commit host over a text table; `current` toggles `isCurrent`, `evaluates` what evaluate() answers. */
function fakeHost(overrides = {}) {
  const texts = { [APP]: APP_TEXT, 'theme/theme.json': THEME };
  const applied = [];
  const host = {
    texts,
    applied,
    current: true,
    evaluates: true,
    evaluations: 0,
    text: (path) => texts[path] ?? '',
    isCurrent() {
      return host.current;
    },
    applyEdits(path, changes, label) {
      applied.push({ path, changes, label });
      texts[path] = applyChanges(texts[path], changes);
    },
    async evaluate() {
      host.evaluations += 1;
      return host.evaluates;
    },
    theme: () => ({ path: 'theme/theme.json', text: texts['theme/theme.json'], themeId: 'light' }),
    ...overrides,
  };
  return host;
}

const NODE = '42';
const inline = { node: NODE, nodeLabel: 'Button', destination: 'inline' };

// ---- set, clear, revert ---------------------------------------------------------------------------

await check('set: an accepted value is one override, undoable, with a menu label', () => {
  const module = fakeModule();
  const client = clientFor(module);
  const result = client.set(NODE, 'color', '#ff0000', 'Set color on Button');
  assert.equal(result.ok, true);
  assert.equal(result.changed, true);
  assert.deepEqual(result.overrides, [{ node: NODE, property: 'color', value: '#ff0000' }]);
  const state = client.state();
  assert.equal(state.canUndo, true);
  assert.equal(state.undoLabel, 'Set color on Button');
  assert.equal(state.canRedo, false);
});

await check('set: setting the same value twice is no second history entry', () => {
  const client = clientFor(fakeModule());
  client.set(NODE, 'color', 'red');
  const again = client.set(NODE, 'color', 'red');
  assert.equal(again.changed, false);
  client.undo();
  assert.equal(client.state().canUndo, false, 'one step only');
});

await check('set: a refused value shows its TESSERA_CSS code and changes nothing', () => {
  const module = fakeModule();
  const client = clientFor(module);
  const result = client.set(NODE, 'colour', 'red');
  assert.equal(result.ok, false);
  assert.equal(result.reason, 'invalid_override');
  assert.equal(result.diagnostics[0].code, 'TESSERA_CSS_UNKNOWN_PROPERTY');
  assert.equal(client.state().canUndo, false, 'a refusal is not a history entry');
  assert.deepEqual(client.state().overrides, []);
});

await check('clear one property, then clear every override of the node', () => {
  const client = clientFor(fakeModule());
  client.set(NODE, 'color', 'red');
  client.set(NODE, 'padding-left', '8px');
  client.set('7', 'color', 'blue');
  client.clear(NODE, 'color');
  assert.deepEqual(
    client.state().overrides.map((entry) => `${entry.node}:${entry.property}`).sort(),
    ['42:padding-left', '7:color'],
  );
  client.clear(NODE);
  assert.deepEqual(client.state().overrides.map((entry) => entry.node), ['7'], 'other nodes keep theirs');
  assert.equal(client.state().undoLabel, 'Clear overrides');
});

await check('revert removes every override of every node, and undo brings them back', () => {
  const client = clientFor(fakeModule());
  client.set(NODE, 'color', 'red');
  client.set('7', 'color', 'blue');
  client.revert();
  assert.deepEqual(client.state().overrides, []);
  assert.equal(client.state().undoLabel, 'Revert all overrides');
  client.undo();
  assert.equal(client.state().overrides.length, 2);
  assert.equal(client.state().redoLabel, 'Revert all overrides');
  client.redo();
  assert.deepEqual(client.state().overrides, []);
});

await check('undo and redo walk the labels in order; a new operation drops the redo stack', () => {
  const client = clientFor(fakeModule());
  client.set(NODE, 'color', 'red', 'Set color on Button');
  client.set(NODE, 'padding-left', '4px', 'Set padding-left on Button');
  assert.equal(client.state().undoLabel, 'Set padding-left on Button');
  client.undo();
  assert.equal(client.state().undoLabel, 'Set color on Button');
  assert.equal(client.state().redoLabel, 'Set padding-left on Button');
  client.set(NODE, 'margin-left', '1px');
  assert.equal(client.state().canRedo, false);
  assert.equal(client.state().redoLabel, '');
});

await check('a module without the exports degrades to an honest refusal, never a throw', () => {
  const module = fakeModule();
  delete module._tessera_canvas_override;
  const result = clientFor(module).set(NODE, 'color', 'red');
  assert.equal(result.ok, false);
  assert.equal(result.reason, 'unavailable');
  assert.equal(result.diagnostics[0].code, 'TESSERA_DESIGNER_UNAVAILABLE');
});

// ---- the commit round trip ------------------------------------------------------------------------

await check('commit round trip: patch, apply once, evaluate, settle; overrides become the source', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const host = fakeHost();
  client.set(NODE, 'color', 'red');
  const outcome = await client.commit(inline, host);
  assert.deepEqual(outcome, { status: 'committed', edits: 1, files: [APP] });
  // Byte offsets were converted: the edit landed after `<View` although `é` and a surrogate pair precede it.
  assert.ok(host.texts[APP].includes('<View style={{ color: "red" }}>'), host.texts[APP]);
  assert.equal(host.applied.length, 1, 'one applyEdits (one text-history entry) per file');
  assert.equal(host.applied[0].label, 'Commit style to Button');
  assert.equal(host.evaluations, 1);
  assert.deepEqual(module.calls.map((call) => call[0]), ['override', 'patch', 'settle']);
  assert.deepEqual(client.state().overrides, []);
  assert.equal(client.state().canUndo, false, 'after a commit the node has nothing to undo on the canvas');
});

await check('commit order: patch before apply, evaluate before settle', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const order = [];
  const host = fakeHost({
    applyEdits(path, changes) {
      order.push('apply');
      host.texts[path] = applyChanges(host.texts[path], changes);
    },
    async evaluate() {
      order.push('evaluate');
      return true;
    },
  });
  client.set(NODE, 'color', 'red');
  const originalPatch = module._tessera_canvas_patch;
  module._tessera_canvas_patch = (...args) => (order.push('patch'), originalPatch(...args));
  const originalSettle = module._tessera_canvas_settle;
  module._tessera_canvas_settle = (...args) => (order.push('settle'), originalSettle(...args));
  await client.commit(inline, host);
  assert.deepEqual(order, ['patch', 'apply', 'evaluate', 'settle']);
});

await check('commit leaves another node\'s overrides and history alone (D3: per-node settle)', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  client.set('7', 'color', 'blue', 'Set color on Card');
  client.set(NODE, 'color', 'red', 'Set color on Button');
  await client.commit(inline, fakeHost());
  const state = client.state();
  assert.deepEqual(state.overrides, [{ node: '7', property: 'color', value: 'blue' }]);
  assert.equal(state.canUndo, true, 'the other node\'s step survives the settle');
  assert.equal(state.undoLabel, 'Set color on Card', 'and the committed node\'s step is gone from the labels');
  client.undo();
  assert.deepEqual(client.state().overrides, [], 'undo reverts the other node only; the committed one never comes back');
});

await check('settle history, old module: without canUndo in the settle report the labels decide', async () => {
  const module = fakeModule({ oldSettle: true });
  const client = clientFor(module);
  client.set(NODE, 'color', 'red');
  await client.commit(inline, fakeHost());
  assert.equal(client.state().canUndo, false);
});

await check('mismatch keeps the overrides and their history, and reports the diagnostic', async () => {
  const module = fakeModule();
  module.state.settleWith = 'mismatch';
  const client = clientFor(module);
  client.set(NODE, 'color', 'red');
  const outcome = await client.commit(inline, fakeHost());
  assert.equal(outcome.status, 'mismatch');
  assert.equal(outcome.diagnostics[0].code, 'TESSERA_DESIGNER_PATCH_MISMATCH');
  assert.deepEqual(client.state().overrides, [{ node: NODE, property: 'color', value: 'red' }]);
  assert.equal(client.state().canUndo, true, 'the override history is untouched');
  client.undo();
  assert.deepEqual(client.state().overrides, [], 'and undo still works');
});

await check('a rejected patch (buffer moved since the last evaluation) applies nothing and touches no history', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const host = fakeHost();
  host.current = false;
  client.set(NODE, 'color', 'red');
  const outcome = await client.commit(inline, host);
  assert.equal(outcome.status, 'rejected');
  assert.equal(outcome.reason, 'version_mismatch');
  assert.equal(host.applied.length, 0, 'no text-history entry');
  assert.equal(host.evaluations, 0);
  assert.equal(module.calls.some((call) => call[0] === 'settle'), false);
  assert.equal(client.state().overrides.length, 1);
  assert.equal(client.state().undoLabel, 'Set color');
  // The developer can commit again after the preview catches up.
  host.current = true;
  assert.equal((await client.commit(inline, host)).status, 'committed');
});

await check('a patch edit off a character boundary is rejected before any buffer is touched', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const host = fakeHost();
  client.set(NODE, 'color', 'red');
  const real = module._tessera_canvas_patch;
  const euro = Buffer.byteLength('// h') + 1; // inside the two bytes of `é`
  module._tessera_canvas_patch = (...args) => {
    const pointer = real(...args);
    const patch = JSON.parse(module.UTF8ToString(pointer));
    patch.edits[0].offset = euro;
    module.stringToUTF8(JSON.stringify(patch), pointer);
    return pointer;
  };
  const outcome = await client.commit(inline, host);
  assert.equal(outcome.status, 'rejected');
  assert.equal(outcome.reason, 'edit_out_of_range');
  assert.equal(host.applied.length, 0);
  assert.equal(host.texts[APP], APP_TEXT);
});

await check('a commit with no overrides answers no-patch with the spec reason', async () => {
  const outcome = await clientFor(fakeModule()).commit(inline, fakeHost());
  assert.equal(outcome.status, 'no-patch');
  assert.equal(outcome.reason, 'no_overrides');
});

await check('no second commit for a node while one is open', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  let release;
  const gate = new Promise((resolve) => (release = resolve));
  const host = fakeHost({
    async evaluate() {
      await gate;
      return true;
    },
  });
  client.set(NODE, 'color', 'red');
  const first = client.commit(inline, host);
  const second = await client.commit(inline, host);
  assert.deepEqual(second, { status: 'busy' });
  release();
  assert.equal((await first).status, 'committed');
});

await check('a source that does not compile leaves the commit unsettled until the next good evaluation', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const host = fakeHost();
  host.evaluates = false;
  client.set(NODE, 'color', 'red');
  const outcome = await client.commit(inline, host);
  assert.equal(outcome.status, 'unsettled');
  assert.equal(client.pendingSettle(), NODE);
  assert.equal(client.state().overrides.length, 1, 'kept until settled');
  assert.equal(module.calls.some((call) => call[0] === 'settle'), false);
  assert.equal((await client.commit(inline, host)).status, 'busy', 'still the same open commit');
  const late = client.evaluated();
  assert.equal(late.status, 'committed');
  assert.equal(client.pendingSettle(), null);
  assert.deepEqual(client.state().overrides, []);
});

await check('token commit hands the theme buffer over first and patches the theme asset', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  const host = fakeHost();
  client.set(NODE, 'color', 'var(--accent)');
  const outcome = await client.commit(
    { node: NODE, nodeLabel: 'Button', destination: 'token', token: '--accent', tokenValue: '#ff0000' },
    host,
  );
  assert.equal(outcome.status, 'committed');
  assert.deepEqual(outcome.files, ['theme/theme.json']);
  const themeCall = module.calls.findIndex((call) => call[0] === 'set_theme');
  const patchCall = module.calls.findIndex((call) => call[0] === 'patch');
  assert.ok(themeCall >= 0 && themeCall < patchCall, 'hand-over precedes the patch');
  assert.deepEqual(module.calls[themeCall], ['set_theme', 'theme/theme.json', THEME, 'light']);
  assert.deepEqual(module.calls[patchCall].slice(1), [NODE, 'token', '', '', '--accent', '#ff0000']);
  assert.ok(host.texts['theme/theme.json'].includes('"--accent":"#ff0000"'));
  assert.equal(host.applied[0].label, 'Commit style to Button');
});

await check('token commit without a theme asset keeps the module\'s not_token_bound answer', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  client.set(NODE, 'color', 'var(--accent)');
  const outcome = await client.commit(
    { node: NODE, nodeLabel: 'Button', destination: 'token', token: '--accent', tokenValue: '#f00' },
    fakeHost({ theme: () => null }),
  );
  assert.deepEqual([outcome.status, outcome.reason], ['no-patch', 'not_token_bound']);
  assert.deepEqual(module.calls.find((call) => call[0] === 'set_theme'), ['set_theme', '', '', ''], 'cleared, not stale');
});

await check('class destination forwards the class name and file', async () => {
  const module = fakeModule();
  const client = clientFor(module);
  client.set(NODE, 'color', 'red');
  await client.commit({ ...inline, destination: 'class', className: 'card', file: 'styles/theme.css' }, fakeHost());
  assert.deepEqual(module.calls.find((call) => call[0] === 'patch').slice(1), [NODE, 'class', 'card', 'styles/theme.css', '', '']);
});

await check('reset (restart) drops overrides, history and a pending settle', async () => {
  const client = clientFor(fakeModule());
  client.set(NODE, 'color', 'red');
  const host = fakeHost();
  host.evaluates = false;
  await client.commit(inline, host);
  client.reset();
  assert.deepEqual(client.state(), { canUndo: false, canRedo: false, undoLabel: '', redoLabel: '', overrides: [] });
  assert.equal(client.pendingSettle(), null);
});

await check('subscribers hear every change', () => {
  const client = clientFor(fakeModule());
  let heard = 0;
  const stop = client.subscribe(() => (heard += 1));
  client.set(NODE, 'color', 'red');
  client.undo();
  stop();
  client.redo();
  assert.equal(heard, 2);
});

// ---- UTF-8 edits ------------------------------------------------------------------------------------

await check('utf8 edits: byte ranges become UTF-16 ranges across two- and four-byte characters', () => {
  const text = 'aé\u{1d11e}b'; // a, é (2 bytes), 𝄞 (4 bytes, 2 UTF-16 units), b
  const [change] = toTextChanges(text, [{ offset: 7, length: 1, newText: 'X' }]);
  assert.deepEqual(change, { from: 4, to: 5, insert: 'X' });
  assert.equal(applyChanges(text, [change]), 'aé\u{1d11e}X');
});

await check('utf8 edits: a split character or an offset past the end is refused', () => {
  const text = 'aéb';
  assert.throws(() => toTextChanges(text, [{ offset: 2, length: 0, newText: '' }]), EditRangeError);
  assert.throws(() => toTextChanges(text, [{ offset: 99, length: 0, newText: '' }]), EditRangeError);
});

await check('utf8 edits: descending application keeps offsets honest, overlap is refused', () => {
  const text = 'abcdef';
  const changes = [
    { from: 1, to: 2, insert: 'X' },
    { from: 4, to: 4, insert: 'YY' },
  ];
  assert.equal(applyChanges(text, changes), 'aXcdYYef');
  assert.throws(() => applyChanges(text, [{ from: 1, to: 4, insert: '' }, { from: 3, to: 5, insert: '' }]), EditRangeError);
});

// ---- colour helper ------------------------------------------------------------------------------------

await check('hexOf reads the three forms the colour control can show and nothing else', () => {
  assert.equal(hexOf('#ABC'), '#aabbcc');
  assert.equal(hexOf('#FF8800'), '#ff8800');
  assert.equal(hexOf('rgb(255, 136, 0)'), '#ff8800');
  assert.equal(hexOf('rgba(1,2,3,0.5)'), null);
  assert.equal(hexOf('rgb(300,0,0)'), null);
  assert.equal(hexOf('red'), null);
  assert.equal(hexOf('var(--accent)'), null);
  assert.equal(isColorProperty('background-color') && isColorProperty('color') && !isColorProperty('padding-left'), true);
});

// ---- D3 routing -------------------------------------------------------------------------------------

await check('D3 routing table: where Undo goes for every focus and last-acted domain', () => {
  const table = [
    // focus, last, expected target
    ['editor', null, 'text'],
    ['editor', 'override', 'text'],
    ['canvas', null, 'override'],
    ['canvas', 'text', 'override'],
    ['inspector', 'text', 'override'],
    ['text-input', 'override', 'native'],
    ['text-input', 'text', 'native'],
    ['other', 'override', 'override'],
    ['other', 'text', 'text'],
    ['other', null, 'text'],
  ];
  for (const [focus, last, expected] of table) {
    assert.equal(targetFor(focus, last), expected, `${focus}/${last}`);
  }
});

function domains(log, overrides = {}, texts = {}) {
  return {
    override: { canUndo: true, canRedo: true, undoLabel: 'Set color on Button', redoLabel: 'Clear color on Button', undo: () => (log.push('override.undo'), true), redo: () => (log.push('override.redo'), true), ...overrides },
    text: { canUndo: true, canRedo: true, undoLabel: 'typing', redoLabel: 'typing', undo: () => (log.push('text.undo'), true), redo: () => (log.push('text.redo'), true), ...texts },
  };
}

await check('D3 routing: runEdit calls only the target\'s history and never a native one', () => {
  const log = [];
  assert.equal(runEdit('undo', 'canvas', null, domains(log)).target, 'override');
  assert.equal(runEdit('redo', 'editor', 'override', domains(log)).target, 'text');
  assert.equal(runEdit('undo', 'other', 'text', domains(log)).target, 'text');
  const native = runEdit('undo', 'text-input', 'override', domains(log));
  assert.deepEqual(native, { target: 'native', changed: false });
  assert.deepEqual(log, ['override.undo', 'text.redo', 'text.undo']);
});

await check('D3 menu labels name the target, and an empty stack is disabled', () => {
  const full = { override: { canUndo: true, canRedo: true, undoLabel: 'Set color on Button', redoLabel: 'Revert all overrides' }, text: { canUndo: true, canRedo: false, undoLabel: 'typing', redoLabel: '' } };
  assert.deepEqual(menuState('override', full), { target: 'override', canUndo: true, canRedo: true, undoTitle: 'Undo Set color on Button', redoTitle: 'Redo Revert all overrides' });
  const text = menuState('text', full);
  assert.deepEqual([text.undoTitle, text.canRedo, text.redoTitle], ['Undo typing', false, 'Redo']);
  const empty = menuState('override', { override: { canUndo: false, canRedo: false, undoLabel: '', redoLabel: '' }, text: full.text });
  assert.deepEqual([empty.canUndo, empty.undoTitle], [false, 'Undo']);
  assert.deepEqual(menuState('native', full), { target: 'native', canUndo: true, canRedo: true, undoTitle: 'Undo', redoTitle: 'Redo' });
});

// ---- the command table -------------------------------------------------------------------------------

const ev = (key, extra = {}) => ({ key, ctrlKey: false, metaKey: false, altKey: false, shiftKey: false, ...extra });

await check('commands: edit.undo/edit.redo keep their ids, bind only where the focus is not a text field', () => {
  const context = createContext({ 'project.open': true, 'edit.target': 'override', 'edit.canUndo': true, 'edit.canRedo': true });
  const registry = createRegistry(COMMANDS, { context });
  registry.register('edit.undo', () => {});
  registry.register('edit.redo', () => {});
  const keymap = registry.keymap();
  for (const target of ['override', 'text']) {
    assert.equal(keymap.resolve(ev('z', { ctrlKey: true }), [], { 'edit.target': target }).id, 'edit.undo', target);
    assert.equal(keymap.resolve(ev('y', { ctrlKey: true }), [], { 'edit.target': target }).id, 'edit.redo', target);
    assert.equal(keymap.resolve(ev('z', { ctrlKey: true, shiftKey: true }), [], { 'edit.target': target }).id, 'edit.redo', target);
  }
  assert.equal(keymap.resolve(ev('z', { ctrlKey: true }), [], { 'edit.target': 'native' }).status, 'none', 'a text field keeps its own undo');
});

await check('commands: the Edit menu shows the target in its label and disables (but keeps) an empty stack', () => {
  const doc = JSON.parse(readFileSync(resolve(ROOT, 'designer/web/src/workbench/menus.json'), 'utf8'));
  const edit = doc.menus.find((menu) => menu.id === 'edit');
  const build = (values) => {
    const registry = createRegistry(COMMANDS, { context: createContext({ 'project.open': true, ...values }) });
    registry.register('edit.undo', () => {});
    registry.register('edit.redo', () => {});
    const items = buildItems(edit.items.filter((item) => item.command === 'edit.undo' || item.command === 'edit.redo'), { registry, keyPrefix: 'edit' });
    return Object.fromEntries(items.filter((item) => item.command).map((item) => [item.command, item]));
  };
  const live = build({ 'edit.canUndo': true, 'edit.canRedo': false, 'edit.undoTitle': 'Undo Set color on Button', 'edit.redoTitle': 'Redo' });
  assert.equal(live['edit.undo'].label, 'Undo Set color on Button');
  assert.equal(live['edit.undo'].enabled, true);
  assert.equal(live['edit.redo'].enabled, false, 'an empty redo stack is disabled; the menu renders it aria-disabled and focusable');
  assert.equal(live['edit.redo'].label, 'Redo');
  const none = build({ 'edit.canUndo': false, 'edit.canRedo': false });
  assert.equal(none['edit.undo'].label, 'Undo', 'no title key value: the plain title');
});

process.stdout.write(failures === 0 ? '\noverride client: all cases passed\n' : `\noverride client: ${failures} failed\n`);
process.exit(failures === 0 ? 0 : 1);
