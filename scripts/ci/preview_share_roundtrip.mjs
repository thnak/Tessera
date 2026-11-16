// SPDX-License-Identifier: MIT
//
// Round-trip test for the P5 share-link codec (issue #169; the format is docs/specs/
// designer-studio-v1.md section 7.1, the codec designer/web/share.js).
//
//   node scripts/ci/preview_share_roundtrip.mjs
//
// What this covers, and what it deliberately does not:
//
//   * every branch of `encodeShare`/`decodeShare` that does not need a DOM: the compressed path
//     (`z`), the gzip path (`g`), and the uncompressed fallback (`u`) that older browsers use;
//   * the envelope fields, the per-field validation, the version refusal, and the two size caps;
//   * determinism (the same state must produce the same fragment, or a shared link would churn);
//   * the real samples from `samples/`, read from disk, so the claim "the samples fit a link" is
//     measured against the shipped bytes rather than a fixture.
//
// It does NOT cover the page: `designer/web/app.js` and `scene.js` wire these functions to the DOM
// and to `location`, and that wiring is what the Pages lane exercises when it assembles and serves
// the site. Claiming otherwise would be claiming browser behaviour no run here observes.
//
// `globalThis.CompressionStream` exists in Node 18+, so the compressed branches are real code paths
// here, not simulations. The uncompressed path is reached with `{compress: false}`, which is also how
// the page's own feature fallback is chosen.

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '..', '..');
const SHARE_URL = new URL('../../designer/web/share.js', import.meta.url);
const STATE_URL = new URL('../../designer/web/preview-state.js', import.meta.url);

const share = await import(SHARE_URL.href);
// The state vocabulary is its own module (the toolbar uses it without importing the codec), so the
// test imports both rather than reaching through `share` for a re-export that does not exist.
const state = await import(STATE_URL.href);

let failures = 0;
let passed = 0;

async function test(name, body) {
  try {
    await body();
    passed += 1;
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

async function expectShareError(code, body) {
  try {
    await body();
  } catch (error) {
    assert.ok(error instanceof share.ShareError, `expected a ShareError, got ${error}`);
    assert.equal(error.code, code, `expected code ${code}, got ${error.code}`);
    return;
  }
  assert.fail(`expected a ShareError(${code}), nothing was thrown`);
}

const SAMPLE_SOURCE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

const BASE_STATE = {
  source: SAMPLE_SOURCE,
  file: 'ui/Preview.tsx',
  width: 390,
  height: 844,
  dpr: 2,
  theme: 'dark',
};

// One representative state per toolbar combination, so a new preset that forgets the codec fails here.
function states() {
  const out = [];
  for (const [width, height] of state.VIEWPORTS.map(([w, h]) => [w, h])) {
    for (const dpr of state.DPRS) {
      for (const theme of state.THEMES) {
        out.push({ ...BASE_STATE, width, height, dpr, theme });
      }
    }
  }
  return out;
}

await test('compressed round trip preserves every field', async () => {
  const payload = await share.encodeShare(BASE_STATE);
  assert.equal(payload[0], 'z', 'a browser with deflate-raw must produce the compressed mode');
  const back = await share.decodeShare(payload);
  assert.deepEqual(back, BASE_STATE);
});

await test('every viewport / DPR / theme combination round-trips', async () => {
  for (const state of states()) {
    const back = await share.decodeShare(await share.encodeShare(state));
    assert.deepEqual(back, state, `round trip failed for ${state.width}x${state.height}`);
  }
});

await test('the uncompressed fallback round-trips as mode u', async () => {
  const payload = await share.encodeShare(BASE_STATE, { compress: false });
  assert.equal(payload[0], 'u');
  assert.deepEqual(await share.decodeShare(payload), BASE_STATE);
});

await test('the gzip mode is readable (the second fallback)', async () => {
  // Built here with the platform gzip rather than through `encodeShare`, so the `g` decode branch is
  // exercised even though this runtime prefers `deflate-raw` and would never emit it.
  const json = JSON.stringify({ v: 1, s: SAMPLE_SOURCE, w: 320, h: 240, d: 1, t: 'light' });
  const stream = new Blob([new TextEncoder().encode(json)])
    .stream()
    .pipeThrough(new CompressionStream('gzip'));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  let binary = '';
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  const body = btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  const back = await share.decodeShare('g' + body);
  assert.equal(back.source, SAMPLE_SOURCE);
  assert.equal(back.width, 320);
  assert.equal(back.theme, 'light');
});

await test('the same state always produces the same fragment', async () => {
  const first = await share.encodeShare(BASE_STATE);
  const second = await share.encodeShare({ ...BASE_STATE });
  assert.equal(first, second, 'a shared link must not churn between two runs');
});

await test('the compression is real: compressed is smaller than the fallback', async () => {
  const compressed = await share.encodeShare(BASE_STATE);
  const plain = await share.encodeShare(BASE_STATE, { compress: false });
  assert.ok(
    compressed.length < plain.length,
    `compressed ${compressed.length} chars must beat the base64 fallback's ${plain.length}`,
  );
  assert.ok(!compressed.includes('useState'), 'the source must not appear in cleartext');
});

await test('multi-byte source text round-trips', async () => {
  const state = { ...BASE_STATE, source: '// cafÃ© â€” æ—¥æœ¬èªž â€” ðŸŽ¨\nexport function X(): Node { return <View />; }\n' };
  assert.deepEqual(await share.decodeShare(await share.encodeShare(state)), state);
});

await test('an empty source round-trips (a blank project is a valid link)', async () => {
  const state = { ...BASE_STATE, source: '' };
  assert.deepEqual(await share.decodeShare(await share.encodeShare(state)), state);
});

await test('a non-default file name is carried; the default one is omitted', async () => {
  const custom = { ...BASE_STATE, file: 'ui/Other.tsx' };
  assert.equal((await share.decodeShare(await share.encodeShare(custom))).file, 'ui/Other.tsx');
  // The default name must not cost bytes in the fragment: the same source with the default file is
  // strictly shorter than with a named one.
  const plain = await share.encodeShare({ ...BASE_STATE, file: state.DEFAULT_FILE });
  assert.ok(plain.length < (await share.encodeShare(custom)).length);
  assert.equal((await share.decodeShare(plain)).file, state.DEFAULT_FILE);
});

await test('a whole pasted URL and a bare fragment both decode', async () => {
  const payload = await share.encodeShare(BASE_STATE);
  assert.deepEqual(await share.decodeShare(`https://thnak.github.io/Tessera/#p5=${payload}`), BASE_STATE);
  assert.deepEqual(await share.decodeShare(`#p5=${payload}`), BASE_STATE);
  assert.deepEqual(await share.decodeShare(payload), BASE_STATE);
});

await test('stateFromHash ignores a hash without a P5 fragment', async () => {
  assert.equal(await share.stateFromHash(''), null);
  assert.equal(await share.stateFromHash('#section-2'), null);
  assert.equal(await share.stateFromHash(undefined), null);
  const payload = await share.encodeShare(BASE_STATE);
  assert.deepEqual(await share.stateFromHash(`#p5=${payload}`), BASE_STATE);
});

await test('an unknown format version is refused, not guessed', async () => {
  const bad = Buffer.from(JSON.stringify({ v: 2, s: '', w: 320, h: 240, d: 1, t: 'system' }))
    .toString('base64')
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=+$/, '');
  await expectShareError('version', () => share.decodeShare('u' + bad));
});

await test('malformed fragments are refused with a code', async () => {
  await expectShareError('malformed', () => share.decodeShare(''));
  await expectShareError('malformed', () => share.decodeShare('#p5='));
  await expectShareError('malformed', () => share.decodeShare('qabcd'));
  await expectShareError('malformed', () => share.decodeShare('u@@@@'));
  await expectShareError('malformed', () =>
    share.decodeShare('u' + Buffer.from('not json').toString('base64url')),
  );
  await expectShareError('malformed', () => share.decodeShare('#p5=zAAAA'));
});

await test('invalid field values are refused, never defaulted', async () => {
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, dpr: 0 }));
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, dpr: 1.5 }));
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, theme: 'sepia' }));
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, width: 333 }));
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, file: '' }));
  await expectShareError('invalid', () => state.normalizeState({ ...BASE_STATE, source: 42 }));
});

await test('a state over the fragment cap is refused, never truncated', async () => {
  // Incompressible enough to exceed 8 KiB: a long run of distinct identifiers.
  let source = 'export function Big(): Node {\n  const x = 0;\n  return (\n    <View>\n';
  for (let index = 0; index < 4000; index += 1) {
    source += `      <Text>row_${index}_${(index * 2654435761) % 1000003}</Text>\n`;
  }
  source += '    </View>\n  );\n}\n';
  await expectShareError('too-large', () => share.encodeShare({ ...BASE_STATE, source }));
  // And the same refusal on the way in, so a hostile or stale link cannot skip the cap.
  await expectShareError('too-large', () => share.decodeShare('z' + 'A'.repeat(share.MAX_FRAGMENT_CHARS)));
});

await test('a decompression bomb is cut off at the envelope cap', async () => {
  // 4 MiB of one repeated byte compresses into the fragment cap but expands past MAX_ENVELOPE_BYTES,
  // so the read loop has to stop it rather than buffer the whole thing.
  const bomb = new Uint8Array(4 * 1024 * 1024).fill(0x20);
  const stream = new Blob([bomb]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  let binary = '';
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  const body = btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  assert.ok(body.length < share.MAX_FRAGMENT_CHARS, 'the bomb must fit the fragment cap to be a bomb');
  assert.ok(bomb.length > state.MAX_ENVELOPE_BYTES, 'and it must expand past the envelope cap');
  await expectShareError('too-large', () => share.decodeShare('z' + body));
});

await test('a shipped sample fits a link with room to spare', async () => {
  const samplePath = resolve(REPO_ROOT, 'samples', 'parity-demo', 'Counter.tsx');
  const source = readFileSync(samplePath, 'utf8');
  const state = { ...BASE_STATE, source, file: 'ui/Counter.tsx' };
  const payload = await share.encodeShare(state);
  assert.deepEqual(await share.decodeShare(payload), state);
  assert.ok(
    payload.length < share.MAX_FRAGMENT_CHARS / 4,
    `samples/parity-demo/Counter.tsx encodes to ${payload.length} chars, ` +
      `which should be far under ${share.MAX_FRAGMENT_CHARS}`,
  );
  process.stdout.write(`     samples/parity-demo/Counter.tsx -> ${payload.length} fragment chars\n`);
});

await test('an unsupported decompressor is reported as such', async () => {
  // The page runs this path when a browser lacks DecompressionStream; simulate by hiding the global
  // for the duration of one call. Restored in `finally` so later tests are unaffected.
  const saved = globalThis.DecompressionStream;
  const payload = await share.encodeShare(BASE_STATE);
  try {
    delete globalThis.DecompressionStream;
    await expectShareError('unsupported', () => share.decodeShare(payload));
  } finally {
    globalThis.DecompressionStream = saved;
  }
});

// --- #392: the pre-merge link format, pinned as a fixture ---------------------------------------
//
// Issue #392 merged the three pages into one `index.html` and turned `scene.html`/`canvas.html` into
// redirect stubs. The acceptance sentence is that a share link made *before* the merge restores the
// same state after it, which needs more than "the codec round-trips against itself": a fragment
// written by the old page has to decode on the new one. The codec is byte-identical across the merge
// (`share.js` and `preview-state.js` were not touched), so the guarantee follows -- but a future edit
// that changed the envelope would break every link in circulation silently, and no self-round-trip
// test would notice. These fixtures are therefore *frozen strings*, produced by the pre-merge code and
// stored here verbatim, not regenerated on each run.
//
// The fixture's fragment was produced by `designer/web/share.js` at commit `201ced54` (origin/main
// before the merge) from the state below, and the state is asserted on the way back out field by
// field. If this test ever fails, the merge broke a link somebody already has.

await test('a fragment produced by the pre-merge page decodes to the same state (#392)', async () => {
  // Produced by the pre-merge `encodeShare` at commit 201ced54 from exactly this state, then recorded
  // here verbatim. It is NOT regenerated on each run: regenerating it would make the test assert only
  // that the current codec agrees with itself, which is the one thing a migration fixture must not do.
  const PRE_MERGE_FRAGMENT =
    'zPY7NasMwEIRfZdiTTAVOmhxSRRa0uefS0kvcQ7BUalJWRV41AeN3L1aKb8PuNz8j_ZJZaxrIULj9xCT4zNxJ' +
    'HxmHmFlCUpXBMfqAsWWgizwITt380xiCFOoDDfIQXuUswfYsTq2qfcuzIQXJiaFmDdj3PlzdXQP2LdzElQSDs' +
    'WROti7HBXnJIpER-fDdd5dmVBUatxSrYsID1tXknr239Z3_99t66Zv3TC2TpiuZzdNK0xeZ3XaryZN51CRkyJ' +
    '_ThaY_';
  const PRE_MERGE_STATE = {
    source: SAMPLE_SOURCE,
    file: 'ui/Preview.tsx',
    width: 390,
    height: 844,
    dpr: 2,
    theme: 'dark',
  };
  assert.deepEqual(
    await share.decodeShare('#p5=' + PRE_MERGE_FRAGMENT),
    PRE_MERGE_STATE,
    'a link made before the merge must restore the same state after it',
  );
});

process.stdout.write(`\n${passed} passed, ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
