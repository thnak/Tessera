// SPDX-License-Identifier: MIT

// The one place the studio page marshals calls into a Wasm module (issue #392).
//
// All three preview modules answer with the same shape: a C export that takes UTF-8 strings followed
// by numbers and returns a pointer to a document the module owns. Before the merge each page carried
// its own copy of that dance (`app.js`, `scene.js`, `canvas.js`), which is three chances to free the
// wrong pointer once the pages share a page instead of living apart. Every view now calls through
// here, so the lifetime rule is written once:
//
//   * every argument string is allocated in the module heap and freed on the way out, success or
//     throw;
//   * the returned document is released with `tessera_free`, the module's documented matching
//     release, and never with `_free`;
//   * a null return is an error, not an empty document.
//
// This module holds no state and touches no DOM: a view passes the instantiated module it already
// has, so a module instance stays owned by the view that created it.

/**
 * Calls `module[name](...strings, ...numbers)` and returns the UTF-8 string the module produced.
 *
 * `strings` are copied into the module's heap (they are the page's text: a source document, a file
 * name, an input script); `numbers` are passed as-is, which is how the sized scene export takes its
 * width, height and DPR and how the canvas export takes a texture-view handle. The returned block is
 * the module's, and `tessera_free` is its matching release.
 */
export function callExport(module, name, strings = [], numbers = []) {
  const pointers = [];
  try {
    for (const text of strings) {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
    }
    const resultPtr = module[name](...pointers, ...numbers);
    if (!resultPtr) {
      // An export that returns 0 is out of memory or refused its input; both are errors, and neither
      // is "an empty document" that the page should render as a successful empty pane.
      throw new Error(name + ' returned no document');
    }
    try {
      return module.UTF8ToString(resultPtr);
    } finally {
      module._tessera_free(resultPtr);
    }
  } finally {
    for (const pointer of pointers) {
      module._free(pointer);
    }
  }
}

/**
 * The names a module must export for a view to be able to call it.
 *
 * Checked once, at load: a module built with a different `EXPORTED_FUNCTIONS` line otherwise fails as
 * "x is not a function" on the first Run, which is the least useful moment to find out and the least
 * useful message to read.
 */
const REQUIRED_EXPORTS = Object.freeze([
  '_malloc',
  '_free',
  '_tessera_free',
  'stringToUTF8',
  'UTF8ToString',
  'lengthBytesUTF8',
]);

/** Throws a readable error naming the first required export `module` is missing, or returns it. */
export function requireExports(module, extra = []) {
  for (const name of [...REQUIRED_EXPORTS, ...extra]) {
    if (typeof module[name] !== 'function') {
      throw new Error('the module is missing `' + name + '`');
    }
  }
  return module;
}
