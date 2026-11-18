// SPDX-License-Identifier: MIT

// The one place the studio page marshals calls into a Wasm module (issue #392).
//
// All three preview modules answer with the same shape: a C export that takes UTF-8 strings followed
// by numbers and returns a pointer to a document the module owns. Before the merge each page carried
// its own copy of that dance, which is three chances to free the wrong pointer once the pages share a
// page instead of living apart. Every view now calls through here, so the lifetime rule is written
// once:
//
//   * every argument string is allocated in the module heap and freed on the way out, success or
//     throw;
//   * the returned document is released with `tessera_free`, the module's documented matching
//     release, and never with `_free`;
//   * a null return is an error, not an empty document.
//
// This module holds no state and touches no DOM: a session passes the instantiated module it already
// has, so a module instance stays owned by the session that created it.

/** The Emscripten module surface the three preview modules share. */
export interface WasmModule {
  _malloc(size: number): number;
  _free(pointer: number): void;
  _tessera_free(pointer: number): void;
  lengthBytesUTF8(text: string): number;
  stringToUTF8(text: string, pointer: number, maxBytesToWrite: number): void;
  UTF8ToString(pointer: number): string;
  [name: string]: unknown;
}

/**
 * Calls `module[name](...strings, ...numbers)` and returns the UTF-8 string the module produced.
 *
 * `strings` are copied into the module's heap (they are the page's text: a source document, a file
 * name, an input script); `numbers` are passed as-is, which is how the sized scene export takes its
 * width, height and DPR and how the canvas export takes a texture-view handle. The returned block is
 * the module's, and `tessera_free` is its matching release.
 */
export function callExport(
  module: WasmModule,
  name: string,
  strings: string[] = [],
  numbers: number[] = [],
): string {
  const pointers: number[] = [];
  try {
    for (const text of strings) {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
    }
    const exported = module[name];
    if (typeof exported !== "function") {
      throw new Error(`the module does not export \`${name}\``);
    }
    const resultPtr = (exported as (...args: number[]) => number)(...pointers, ...numbers);
    if (!resultPtr) {
      // An export that returns 0 is out of memory or refused its input; both are errors, and neither
      // is "an empty document" that the page should render as a successful empty pane.
      throw new Error(name + " returned no document");
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

/** What a module is staged with before it previews a project: see `ProjectSnapshot` in `views.ts`. */
export interface StagedProject {
  entry: string | null;
  sources: readonly { path: string; text: string }[];
  stylesheets: readonly { path: string; text: string }[];
}

function callVoid(module: WasmModule, name: string, strings: string[]): void {
  const pointers: number[] = [];
  try {
    for (const text of strings) {
      const length = module.lengthBytesUTF8(text) + 1;
      const pointer = module._malloc(length);
      module.stringToUTF8(text, pointer, length);
      pointers.push(pointer);
    }
    const exported = module[name];
    if (typeof exported !== "function") {
      throw new Error(`the module does not export \`${name}\``);
    }
    (exported as (...args: number[]) => void)(...pointers);
  } finally {
    for (const pointer of pointers) {
      module._free(pointer);
    }
  }
}

/**
 * Stages `project` into `module`, replacing whatever it held (`tessera_project_*`).
 *
 * Every preview export reads the staged project, so a session stages right before it asks. The entry is
 * set only when the project names one: a project without an entry is staged as it is, and the module
 * reports that it names none (there is no fallback to another file).
 */
export function stageProject(module: WasmModule, project: StagedProject): void {
  callVoid(module, "_tessera_project_begin", []);
  for (const source of project.sources) {
    callVoid(module, "_tessera_project_add_source", [source.path, source.text]);
  }
  for (const sheet of project.stylesheets) {
    callVoid(module, "_tessera_project_add_stylesheet", [sheet.path, sheet.text]);
  }
  if (project.entry !== null) {
    callVoid(module, "_tessera_project_set_entry", [project.entry]);
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
  "_malloc",
  "_free",
  "_tessera_free",
  "stringToUTF8",
  "UTF8ToString",
  "lengthBytesUTF8",
]);

/** Throws a readable error naming the first required export `module` is missing, or returns it. */
export function requireExports<T extends object>(module: T, extra: string[] = []): T {
  const table = module as Record<string, unknown>;
  for (const name of [...REQUIRED_EXPORTS, ...extra]) {
    if (typeof table[name] !== "function") {
      throw new Error("the module is missing `" + name + "`");
    }
  }
  return module;
}
