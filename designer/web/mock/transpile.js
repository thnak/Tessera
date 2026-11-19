// SPDX-License-Identifier: MIT

// The lazy TypeScript transpiler for `*.mock.ts` (docs/specs/studio-mock-services-v1.md section 4.1,
// ADR-055 decision 5): esbuild-wasm, pinned in `package.json`, imported only when a project has a mock so
// the cold download and first paint stay as they were.
//
// **Transpile-only.** esbuild strips types and does not check them, which is the spec's rule: a type
// error is not a launch failure. The output is a classic-script IIFE that assigns `__mockModule`, the
// shape `sandbox-source.js` expects, so the worker needs no module syntax and no `eval`.
//
// **Not on the UI thread.** In the browser esbuild-wasm runs its WebAssembly in its own worker
// (`worker: true`, the default there), so a slow compile does not block input. The 200 ms per file cap of
// the spec is enforced by the caller racing `compileMock` with a timer.

let initialized = null;

/**
 * @param {string} text the `.mock.ts` source
 * @param {string} path the project-relative path, for messages
 * @param {{ wasmURL?: string }} [options] where the page serves `esbuild.wasm`; omitted under Node
 * @returns {Promise<string>} the IIFE script
 */
export async function compileMock(text, path, options = {}) {
  const esbuild = await import('esbuild-wasm');
  if (initialized === null) {
    initialized = esbuild.initialize(options.wasmURL === undefined ? {} : { wasmURL: options.wasmURL });
  }
  await initialized;
  try {
    const result = await esbuild.transform(text, {
      loader: 'ts',
      format: 'iife',
      globalName: '__mockModule',
      target: 'es2022',
      sourcefile: path,
      logLevel: 'silent',
    });
    return result.code;
  } catch (cause) {
    const first = Array.isArray(cause.errors) ? cause.errors[0] : undefined;
    throw {
      message: first ? first.text : String(cause.message ?? cause),
      line: first?.location?.line,
      // esbuild columns are 0-based; the console prints 1-based.
      column: first?.location ? first.location.column + 1 : undefined,
    };
  }
}
