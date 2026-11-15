// SPDX-License-Identifier: MIT
//
// DOM smoke test for the P5 page chrome (issue #169): `designer/web/studio.js`, the module both pages
// call to mount the toolbar, the sample gallery and the share button.
//
//   node scripts/ci/preview_studio_smoke.mjs
//
// **What this is, and what it is not.** It is a *stub-DOM* test: a few dozen lines of `document`,
// `fetch`, `localStorage` and `history` stand-ins, good enough to run the real module's code paths and
// assert what it did to them. It is NOT a browser test, and it does not prove the pages render: the
// browser half -- that `index.html`/`scene.html` carry the elements the module looks up, that the CSS
// applies, that `location.hash` round-trips in a real navigation -- is what the Pages lane and a human
// opening https://thnak.github.io/Tessera/ verify. Saying this plainly is the point: a smoke test that
// claimed to be a browser test would be the kind of green that hides a blank page.
//
// What it *does* cover, and why each case is worth having:
//   * the manifest is fetched lazily -- not at import, and not before the gallery is opened -- because
//     P6's `cold_engine_ready_ms` budget would otherwise pay for it;
//   * a gallery pick writes the sample's text into the textarea and reports the sample's path;
//   * the toolbar's three selects are built with the documented option sets and fire a change;
//   * the controls persist to localStorage under one key, and the source never does;
//   * a share link's fragment is produced, put in the address bar, and decoded back to the same state;
//   * a broken fragment shows the link banner with its code, instead of silently loading the default.

import assert from 'node:assert/strict';

let failures = 0;
// `async` + `await body()` is not decoration: the module under test is asynchronous (it awaits
// `fetch` and the codec), so a synchronous `test()` would print "ok" before the body had run and
// report a green suite that asserted nothing.
async function test(name, body) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

// --- a minimal DOM ------------------------------------------------------------------------------

class StubElement {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.children = [];
    this.attributes = {};
    this.listeners = {};
    this._text = '';
    this.value = '';
    this.hidden = false;
    this.disabled = false;
    this.className = '';
    this.dataset = {};
    this.id = '';
  }
  append(...nodes) {
    for (const node of nodes) {
      this.children.push(node);
      if (node && node.tagName === 'OPTION' && node.selected) {
        this.value = node.value;
      }
    }
  }
  replaceChildren(...nodes) {
    this.children = [];
    this.append(...nodes);
  }
  setAttribute(name, value) {
    this.attributes[name] = String(value);
  }
  getAttribute(name) {
    return Object.prototype.hasOwnProperty.call(this.attributes, name)
      ? this.attributes[name]
      : null;
  }
  addEventListener(type, handler) {
    (this.listeners[type] ||= []).push(handler);
  }
  dispatch(type, event = {}) {
    for (const handler of this.listeners[type] || []) {
      handler({ target: this, preventDefault() {}, ...event });
    }
  }
  set textContent(value) {
    this._text = String(value);
    this.children = [];
  }
  get textContent() {
    if (this.children.length === 0) {
      return this._text;
    }
    return this.children.map((child) => child.textContent ?? '').join('');
  }
}

const ELEMENTS = [
  'controls',
  'share',
  'share-status',
  'toggle-gallery',
  'gallery',
  'gallery-list',
  'link-note',
  'source',
  'status',
];

function installDom() {
  const byId = new Map();
  for (const id of ELEMENTS) {
    const element = new StubElement(id === 'source' ? 'textarea' : 'div');
    element.id = id;
    byId.set(id, element);
  }
  const document = {
    getElementById: (id) => byId.get(id) || null,
    createElement: (tag) => new StubElement(tag),
    createTextNode: (text) => {
      const node = new StubElement('#text');
      node.textContent = text;
      return node;
    },
    querySelector: () => null,
  };
  return { document, byId };
}

// --- a stub browser -----------------------------------------------------------------------------

const MANIFEST = {
  schema: 1,
  samples: [
    { path: 'samples/parity-demo/Counter.tsx', component: 'Counter' },
    { path: 'samples/gallery/Gallery.tsx', component: 'Gallery' },
  ],
};

const SAMPLE_TEXT = 'export function Counter(): Node {\n  return <View />;\n}\n';

function installBrowser({ hash = '', failingPath = null } = {}) {
  const fetched = [];
  const storage = new Map();
  const historyCalls = [];
  globalThis.document = installDom().document;
  globalThis.fetch = async (url) => {
    fetched.push(url);
    if (url === './samples.json') {
      return { ok: true, status: 200, json: async () => MANIFEST };
    }
    if (failingPath && url.endsWith(failingPath)) {
      return { ok: false, status: 404, text: async () => '' };
    }
    if (url.endsWith('.tsx')) {
      return { ok: true, status: 200, text: async () => SAMPLE_TEXT };
    }
    return { ok: false, status: 404, text: async () => '' };
  };
  globalThis.localStorage = {
    getItem: (key) => (storage.has(key) ? storage.get(key) : null),
    setItem: (key, value) => storage.set(key, String(value)),
  };
  globalThis.history = {
    replaceState: (state, title, url) => historyCalls.push(url),
  };
  globalThis.location = { origin: 'https://example.test', pathname: '/Tessera/', search: '', hash };
  // Node 21+ exposes `navigator` as a getter-only global, so a plain assignment throws in strict
  // module code. `defineProperty` is how a test replaces a platform global.
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: { clipboard: { writeText: async (text) => historyCalls.push(text) } },
  });
  return { fetched, storage, historyCalls, document: globalThis.document };
}

const tick = () => new Promise((resolve) => setImmediate(resolve));

// --- the cases ----------------------------------------------------------------------------------

const { mountStudio } = await import(
  new URL('../../designer/web/studio.js', import.meta.url).href
);

const STARTUP = 'export function Counter(): Node { return <View />; }\n';

async function mount(options = {}) {
  const env = installBrowser(options);
  const studio = await mountStudio({ source: env.document.getElementById('source'), startup: STARTUP });
  return { studio, ...env };
}

// Each case needs a fresh module-level state, and the module keeps none across `mountStudio` calls
// except through the globals above, which `installBrowser` replaces. That is deliberate: the module
// holds no cache, so a second mount is a second page load.

await test('a fresh page fetches nothing before the gallery is opened', async () => {
  const { fetched } = await mount();
  assert.deepEqual(fetched, [], 'no sample manifest on the startup path (P6 cold budget)');
});

await test('the toolbar builds all three controls with the documented options', async () => {
  const { document } = await mount();
  const controls = document.getElementById('controls');
  assert.equal(controls.children.length, 3, 'viewport, DPR, theme');
  const selects = controls.children.map((field) => field.children.find((c) => c.tagName === 'SELECT'));
  assert.deepEqual(
    selects.map((select) => select.children.length),
    [4, 3, 3],
    'four viewports, three DPRs, three themes',
  );
  assert.deepEqual(
    selects[0].children.map((option) => option.value),
    ['320x240', '390x844', '768x1024', '1280x800'],
  );
  assert.deepEqual(selects[1].children.map((option) => option.value), ['1', '2', '3']);
  assert.deepEqual(selects[2].children.map((option) => option.value), ['system', 'light', 'dark']);
});

await test('opening the gallery fetches the manifest once, then the chosen sample', async () => {
  const { document, fetched, studio } = await mount();
  const toggle = document.getElementById('toggle-gallery');
  toggle.dispatch('click');
  await tick();
  assert.deepEqual(fetched, ['./samples.json'], 'manifest fetched on first open only');

  const list = document.getElementById('gallery-list');
  assert.equal(list.children.length, 2, 'one button per sample');
  assert.match(list.children[0].textContent, /Counter/, 'the title is derived from the path');

  list.children[0].dispatch('click');
  await tick();
  await tick();
  assert.ok(fetched.includes('./samples/parity-demo/Counter.tsx'), 'the sample TSX is fetched');
  assert.equal(document.getElementById('source').value, SAMPLE_TEXT, 'source text is loaded');
  assert.equal(studio.file(), 'samples/parity-demo/Counter.tsx', 'the sample path is the link file');
  assert.ok(document.getElementById('gallery').hidden, 'the gallery closes after a pick');
});

await test('a second open does not refetch the manifest', async () => {
  const { document, fetched } = await mount();
  const toggle = document.getElementById('toggle-gallery');
  toggle.dispatch('click');
  await tick();
  toggle.dispatch('click');
  toggle.dispatch('click');
  await tick();
  const manifestFetches = fetched.filter((url) => url === './samples.json');
  assert.equal(manifestFetches.length, 1, 'the manifest is fetched once per page load');
});

await test('a failed sample fetch is reported in the gallery, not thrown', async () => {
  const { document } = await mount({ failingPath: 'Gallery.tsx' });
  document.getElementById('toggle-gallery').dispatch('click');
  await tick();
  const list = document.getElementById('gallery-list');
  list.children[1].dispatch('click');
  await tick();
  const note = list.children.find((child) => child.className && child.className.includes('gallery-note'))
    || document.getElementById('gallery-list').children.find((c) => c.className === 'gallery-note');
  assert.ok(note, 'the failure is rendered as a note');
  assert.match(note.textContent, /404/);
});

await test('the toolbar persists to localStorage, and the source never does', async () => {
  const { document, storage } = await mount();
  const controls = document.getElementById('controls');
  const viewportSelect = controls.children[0].children.find((c) => c.tagName === 'SELECT');
  viewportSelect.value = '390x844';
  viewportSelect.dispatch('change');
  const stored = JSON.parse(storage.get('tessera.preview.p5'));
  assert.equal(stored.viewport, '390x844', 'the toolbar position is remembered');
  assert.equal(stored.source, undefined, 'the source is never persisted by this module');
});

await test('the share link round-trips through the address bar', async () => {
  const { document, historyCalls, studio } = await mount();
  const controls = document.getElementById('controls');
  const viewportSelect = controls.children[0].children.find((c) => c.tagName === 'SELECT');
  viewportSelect.value = '1280x800';
  viewportSelect.dispatch('change');
  document.getElementById('source').value = SAMPLE_TEXT;

  document.getElementById('share').dispatch('click');
  await tick();
  await tick();

  const url = historyCalls.find((value) => String(value).includes('#p5='));
  assert.ok(url, 'a fragment link was produced');
  const { decodeShare } = await import(
    new URL('../../designer/web/share.js', import.meta.url).href
  );
  const back = await decodeShare(url);
  assert.equal(back.source, SAMPLE_TEXT, 'the source round-trips');
  assert.equal(back.width, 1280);
  assert.equal(back.height, 800);
  assert.equal(back.dpr, 1);
  assert.equal(back.theme, 'system');
  assert.equal(document.getElementById('share-status').textContent, 'link copied');
  assert.equal(studio.shareState().width, 1280);
});

await test('opening a share link restores its source, viewport and theme', async () => {
  const { encodeShare } = await import(
    new URL('../../designer/web/share.js', import.meta.url).href
  );
  const shared = {
    source: 'export function X(): Node { return <Text>hi</Text>; }\n',
    file: 'ui/Preview.tsx',
    width: 768,
    height: 1024,
    dpr: 3,
    theme: 'dark',
  };
  const payload = await encodeShare(shared);
  const { document, studio } = await mount({ hash: `#p5=${payload}` });
  assert.equal(document.getElementById('source').value, shared.source);
  assert.equal(studio.controls().viewport, '768x1024');
  assert.equal(studio.controls().dpr, '3');
  assert.equal(studio.controls().theme, 'dark');
  assert.ok(document.getElementById('link-note').hidden, 'no error banner for a good link');
});

await test('a broken share link shows the banner and keeps the default source', async () => {
  const { document } = await mount({ hash: '#p5=zAAAA' });
  const note = document.getElementById('link-note');
  assert.equal(note.hidden, false, 'the banner is visible');
  assert.match(note.textContent, /could not be opened/);
  assert.match(note.textContent, /malformed/, 'the ShareError code is shown');
  assert.equal(document.getElementById('source').value, STARTUP, 'the default source still loads');
});

process.stdout.write(`\n${failures === 0 ? 'studio smoke: all cases passed' : `${failures} failed`}\n`);
if (failures > 0) {
  process.exitCode = 1;
}
