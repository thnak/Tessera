// SPDX-License-Identifier: MIT

// "May be out of date" advice for a generated file (issue #391, owner decision 2026-11-17).
//
// The native codegen writes one `<Component>.g.hpp` per component (ADR-040: the generated base
// `gen::<Component>Base`). The studio cannot run codegen, so a developer reading that header in the
// view-only editor deserves a hint when the `.tsx` it came from was edited after it was written.
//
// This is a heuristic and says so: a generated header carries no source digest, and a `git checkout` or a
// copy resets modification times, so it can be wrong both ways. It never blocks viewing, saving or the
// preview, and it stays silent whenever the source cannot be resolved by the project's own naming.
//
// No DOM and no imports, so the Node gate drives it directly.

/** The component a generated header names (`ui/Home.g.hpp` -> `Home`), or `''` when it is not one. */
export function generatedComponent(path) {
  const name = path.slice(path.lastIndexOf('/') + 1);
  const match = /^([A-Za-z_][A-Za-z0-9_]*)\.g\.(?:hpp|h)$/.exec(name);
  return match === null ? '' : match[1];
}

/**
 * The project source that declares `component`: a `.tsx`/`.jsx` whose name is the component's, else the one
 * file whose text declares it (`function Home`, `const Home =`). `null` when there is none or it is
 * ambiguous, because a guess would put a wrong badge on a file.
 *
 * @param {string} component
 * @param {Array<{path: string, text: string}>} sources
 */
export function sourceOfComponent(component, sources) {
  const byName = sources.filter((source) => /(^|\/)([^/]+)\.[jt]sx$/.exec(source.path)?.[2] === component);
  if (byName.length === 1) {
    return byName[0];
  }
  if (byName.length > 1) {
    return null;
  }
  const declaration = new RegExp(`(?:function\\s+${component}\\b|(?:const|let|var)\\s+${component}\\s*[=:])`);
  const declaring = sources.filter((source) => declaration.test(source.text));
  return declaring.length === 1 ? declaring[0] : null;
}

/**
 * The generated files whose source is newer than they are.
 *
 * @param {Array<{path: string, kind: string, text: string, modified: number}>} files each with its
 *   modification time (0 when unknown, a zip) and, for a source, its text
 * @returns {string[]} paths of the generated files that may be out of date, sorted
 */
export function staleGenerated(files) {
  const sources = files.filter((file) => file.kind === 'source');
  const stale = [];
  for (const file of files) {
    const component = generatedComponent(file.path);
    if (component === '' || !(file.modified > 0)) {
      continue;
    }
    const source = sourceOfComponent(component, sources);
    if (source !== null && source.modified > 0 && source.modified > file.modified) {
      stale.push(file.path);
    }
  }
  return stale.sort();
}
