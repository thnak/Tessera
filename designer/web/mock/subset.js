// SPDX-License-Identifier: MIT

// The supported TypeScript subset of a `*.mock.ts` file (docs/specs/studio-mock-services-v1.md
// section 3.4), enforced before any compile or run.
//
// **Why a scan and not the transpiler's error.** The transpiler is transpile-only: it accepts `import`,
// `async` and classes happily. The subset is the studio's rule, so it is decided here, once, with the
// stable `TESSERA_MOCK_*` code and a `line:column` the console prints. The scanner is a lexer, not a
// parser: it skips comments, strings, template text and regex literals so a forbidden word inside a
// string is not a violation, and it looks only at identifier tokens, which is all the rules need.
//
// **What this is not.** It is not the sandbox. A hostile mock can spell a forbidden global in ways a
// lexer cannot see (`this['fe' + 'tch']`); the worker (`sandbox-source.js`) removes those globals at
// runtime, and this scan is the early, friendly half that names the common mistakes.
//
// No DOM, no imports: the Node gate and the page run the same function.

/** Section 3.4 constructs and what they are called in section 10. */
const CODE = Object.freeze({
  import: 'TESSERA_MOCK_IMPORT_UNSUPPORTED',
  async: 'TESSERA_MOCK_ASYNC_UNSUPPORTED',
  dynamic: 'TESSERA_MOCK_DYNAMIC_CODE',
  syntax: 'TESSERA_MOCK_SYNTAX',
  sandbox: 'TESSERA_MOCK_SANDBOX_VIOLATION',
});

// Identifiers a mock may not name: DOM, network, wall clock and worker plumbing (section 4.4). `Math`
// is allowed because `Math.random` is replaced by the seeded generator in the worker.
const FORBIDDEN_GLOBALS = new Set([
  'window', 'document', 'self', 'globalThis', 'global', 'navigator', 'location', 'parent', 'top',
  'fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource', 'importScripts', 'postMessage', 'onmessage', 'addEventListener', 'removeEventListener', 'dispatchEvent',
  'Worker', 'SharedWorker', 'BroadcastChannel', 'MessageChannel', 'indexedDB', 'localStorage',
  'sessionStorage', 'caches', 'process', 'Date', 'performance', 'setTimeout', 'setInterval',
  'setImmediate', 'queueMicrotask', 'requestAnimationFrame', 'Atomics', 'SharedArrayBuffer',
  'WebAssembly',
]);

const DYNAMIC_CODE = new Set(['eval', 'Function', 'require']);
const SYNTAX_KEYWORDS = new Set(['class', 'enum']);
// After these a `/` starts a regular expression rather than dividing.
const REGEX_AFTER = new Set(['return', 'typeof', 'case', 'in', 'of', 'delete', 'void', 'throw', 'new']);
const REGEX_AFTER_PUNCT = new Set('(,=:[!&|?{};+-*%<>~^'.split(''));

const IDENT_START = /[A-Za-z_$]/;
const IDENT_PART = /[A-Za-z0-9_$]/;

/**
 * One finding: a stable code, a message for a person and a 1-based position.
 *
 * @typedef {{ code: string, message: string, line: number, column: number }} MockDiagnostic
 */

/**
 * Splits `source` into identifier and punctuator tokens with positions, skipping comments, strings,
 * template literal text and regex literals.
 *
 * @param {string} source
 * @returns {{ tokens: { text: string, ident: boolean, line: number, column: number }[], error: MockDiagnostic | null }}
 */
export function tokenize(source) {
  const tokens = [];
  let i = 0;
  let line = 1;
  let lineStart = 0;
  const n = source.length;
  // Each entry is the brace depth at which a `${` opened inside a template literal.
  const templateStack = [];
  let depth = 0;

  const here = () => ({ line, column: i - lineStart + 1 });
  const newline = () => {
    line += 1;
    lineStart = i + 1;
  };
  const fail = (message, at) => ({
    tokens,
    error: { code: CODE.syntax, message, line: at.line, column: at.column },
  });

  // Consumes template text up to a closing backtick or an opening `${`. Returns `true` when it stopped
  // at `${` (the caller resumes normal scanning), `false` at the closing backtick, `null` at EOF.
  const templateText = () => {
    while (i < n) {
      const c = source[i];
      if (c === '\\') {
        i += 2;
        continue;
      }
      if (c === '\n') {
        newline();
      }
      if (c === '`') {
        i += 1;
        return false;
      }
      if (c === '$' && source[i + 1] === '{') {
        i += 2;
        return true;
      }
      i += 1;
    }
    return null;
  };

  const lastSignificant = () => tokens[tokens.length - 1];

  while (i < n) {
    const c = source[i];
    if (c === '\n') {
      newline();
      i += 1;
    } else if (c === ' ' || c === '\t' || c === '\r') {
      i += 1;
    } else if (c === '/' && source[i + 1] === '/') {
      while (i < n && source[i] !== '\n') {
        i += 1;
      }
    } else if (c === '/' && source[i + 1] === '*') {
      const start = here();
      i += 2;
      while (i < n && !(source[i] === '*' && source[i + 1] === '/')) {
        if (source[i] === '\n') {
          newline();
        }
        i += 1;
      }
      if (i >= n) {
        return fail('Unterminated comment.', start);
      }
      i += 2;
    } else if (c === '"' || c === "'") {
      const start = here();
      i += 1;
      while (i < n && source[i] !== c && source[i] !== '\n') {
        i += source[i] === '\\' ? 2 : 1;
      }
      if (source[i] !== c) {
        return fail('Unterminated string.', start);
      }
      i += 1;
      tokens.push({ text: '""', ident: false, ...start });
    } else if (c === '`') {
      const start = here();
      i += 1;
      const stopped = templateText();
      if (stopped === null) {
        return fail('Unterminated template literal.', start);
      }
      tokens.push({ text: '``', ident: false, ...start });
      if (stopped) {
        templateStack.push(depth);
        depth += 1;
      }
    } else if (c === '}' && templateStack.length > 0 && templateStack[templateStack.length - 1] === depth - 1) {
      const start = here();
      i += 1;
      depth -= 1;
      templateStack.pop();
      const stopped = templateText();
      if (stopped === null) {
        return fail('Unterminated template literal.', start);
      }
      if (stopped) {
        templateStack.push(depth);
        depth += 1;
      }
    } else if (c === '/') {
      const prev = lastSignificant();
      const regex =
        prev === undefined ||
        (prev.ident ? REGEX_AFTER.has(prev.text) : REGEX_AFTER_PUNCT.has(prev.text));
      if (regex) {
        const start = here();
        i += 1;
        let inClass = false;
        while (i < n && source[i] !== '\n' && (inClass || source[i] !== '/')) {
          if (source[i] === '\\') {
            i += 1;
          } else if (source[i] === '[') {
            inClass = true;
          } else if (source[i] === ']') {
            inClass = false;
          }
          i += 1;
        }
        if (source[i] !== '/') {
          return fail('Unterminated regular expression.', start);
        }
        i += 1;
        while (i < n && IDENT_PART.test(source[i])) {
          i += 1;
        }
        tokens.push({ text: '//', ident: false, ...start });
      } else {
        tokens.push({ text: '/', ident: false, ...here() });
        i += 1;
      }
    } else if (IDENT_START.test(c)) {
      const start = here();
      const from = i;
      while (i < n && IDENT_PART.test(source[i])) {
        i += 1;
      }
      tokens.push({ text: source.slice(from, i), ident: true, ...start });
    } else if (c >= '0' && c <= '9') {
      const start = here();
      while (i < n && /[0-9A-Za-z_.]/.test(source[i])) {
        i += 1;
      }
      tokens.push({ text: '0', ident: false, ...start });
    } else {
      if (c === '{') {
        depth += 1;
      } else if (c === '}') {
        depth -= 1;
      }
      const two = source.slice(i, i + 2);
      const text = two === '=>' ? two : c;
      tokens.push({ text, ident: false, ...here() });
      i += text.length;
    }
  }
  return { tokens, error: null };
}

/**
 * Checks the section 3.4 subset and the forbidden identifiers. Returns every finding in source order;
 * an empty array means the file may be compiled.
 *
 * @param {string} source
 * @returns {MockDiagnostic[]}
 */
export function checkSubset(source) {
  const { tokens, error } = tokenize(source);
  /** @type {MockDiagnostic[]} */
  const found = error === null ? [] : [error];
  const add = (token, code, message) =>
    found.push({ code, message, line: token.line, column: token.column });

  for (let k = 0; k < tokens.length; k += 1) {
    const token = tokens[k];
    const prev = tokens[k - 1];
    const next = tokens[k + 1];
    if (token.text === '@') {
      add(token, CODE.syntax, 'Decorators are not supported in a mock.');
      continue;
    }
    if (!token.ident) {
      continue;
    }
    // `obj.class` and `{ class: 1 }` are property names, not the keyword.
    const member = prev !== undefined && prev.text === '.';
    const key = next !== undefined && next.text === ':' && prev !== undefined && (prev.text === '{' || prev.text === ',');
    if (member) {
      if (token.text === 'constructor' || token.text === '__proto__' || token.text === 'prototype') {
        add(token, CODE.sandbox, `Access to .${token.text} is not allowed in a mock.`);
      }
      continue;
    }
    if (key) {
      continue;
    }
    const word = token.text;
    if (word === 'import') {
      add(token, CODE.import, 'Imports are not supported in a mock (v1 modules have no imports).');
    } else if (word === 'export') {
      if (next === undefined || next.text !== 'default') {
        add(token, CODE.import, 'A mock has exactly one export: `export default defineMock(...)`.');
      }
    } else if (word === 'async' || word === 'await' || word === 'yield') {
      add(token, CODE.async, `\`${word}\` is not supported: mock handlers are synchronous.`);
    } else if (word === 'function' && next !== undefined && next.text === '*') {
      add(token, CODE.async, 'Generators are not supported: mock handlers are synchronous.');
    } else if (DYNAMIC_CODE.has(word)) {
      add(token, CODE.dynamic, `\`${word}\` is not allowed in a mock.`);
    } else if (SYNTAX_KEYWORDS.has(word)) {
      add(token, CODE.syntax, `\`${word}\` is not supported in a mock.`);
    } else if (word === 'namespace' && next !== undefined && next.ident) {
      add(token, CODE.syntax, '`namespace` is not supported in a mock.');
    } else if (FORBIDDEN_GLOBALS.has(word)) {
      add(token, CODE.sandbox, `\`${word}\` is not available in the mock sandbox.`);
    }
  }
  return found;
}

/** The identifiers the sandbox shadows or deletes, so the worker source and the scan agree. */
export const FORBIDDEN_IDENTIFIERS = Object.freeze([...FORBIDDEN_GLOBALS]);
