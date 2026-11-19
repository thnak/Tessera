// SPDX-License-Identifier: MIT

// The source text of the mock worker (docs/specs/studio-mock-services-v1.md section 4).
//
// The studio makes a blob from `buildWorkerSource(compiled)` and starts a dedicated worker from it. The
// text is one script: a **prelude** that runs first and a **compiled mock module** after it. The order is
// the sandbox:
//
//   1. the prelude captures the few things it needs (`postMessage`, the message hook, `Math`);
//   2. it replaces every forbidden global with a throwing stub (own properties of the global object, so
//      the stub shadows the inherited one) and the `constructor` of every function kind, so
//      `(() => 0).constructor("...")` is not a way back to `Function`, `eval` or `new Function`;
//   3. only then does the mock module run, with `defineMock` in scope.
//
// **What this proves and what it does not.** It removes the ambient authority the spec lists (network,
// clock, DOM, dynamic code, worker plumbing) from a worker that already has no DOM. It cannot prove the
// network boundary: that rests on the CSP (`connect-src 'self'`, no `unsafe-eval`) the studio page sends
// and on the security review the spec requires before this ships (section 9). The Node gate runs this
// exact text in a `worker_threads` worker; the browser lane (when it exists) runs it in a blob worker.
//
// **No `eval`, no `new Function`, no `data:`** in *this* file either: the source is a template string
// that the host turns into a blob URL, which is the worker's script, not evaluated text.
//
// The mock module's contract with the prelude: `compiled` is a classic script that assigns
// `var __mockModule = { default: <value of defineMock(...)> }` (esbuild `format: 'iife'`,
// `globalName: '__mockModule'`), so the prelude can read the default export without module syntax.

import { FORBIDDEN_IDENTIFIERS } from './subset.js';

// Written as a function body so the project's own tooling parses and formats it; `String(fn)` is what
// reaches the worker, so it must not close over anything: every input arrives through `__config`.
function prelude(__config) {
  'use strict';
  const post = globalThis.postMessage.bind(globalThis);
  // The message listener is registered before the lockdown, because `addEventListener` is itself
  // locked afterwards; `onMessage` is filled in below.
  let onMessage = () => {};
  globalThis.addEventListener('message', (event) => onMessage(event));
  const jsonStringify = JSON.stringify;
  const objectDefine = Object.defineProperty;
  const objectKeys = Object.keys;
  const objectFreeze = Object.freeze;
  const mathObject = Math;

  // --- seeded PRNG (mulberry32): the only randomness a mock can see -----------------------------
  let rngState = 0;
  const seedRng = () => {
    rngState = __config.seed >>> 0;
  };
  const random = () => {
    rngState = (rngState + 0x6d2b79f5) >>> 0;
    let t = rngState;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  seedRng();
  objectDefine(mathObject, 'random', { value: random, writable: false, configurable: false });

  // --- lockdown ---------------------------------------------------------------------------------
  const violation = (name) =>
    new Error('TESSERA_MOCK_SANDBOX_VIOLATION: `' + name + '` is not available in the mock sandbox');
  const thrower = (name) =>
    new Proxy(function sandboxStub() {}, {
      apply() {
        throw violation(name);
      },
      construct() {
        throw violation(name);
      },
      get() {
        throw violation(name);
      },
      set() {
        throw violation(name);
      },
    });
  const lock = (target, name) => {
    try {
      objectDefine(target, name, { value: thrower(name), writable: false, configurable: false });
    } catch (_ignored) {
      // A non-configurable own property already there: nothing to shadow.
    }
  };
  for (const name of __config.forbidden) {
    lock(globalThis, name);
  }
  lock(globalThis, 'eval');
  lock(globalThis, 'Function');
  lock(globalThis, 'require');
  lock(globalThis, 'WebAssembly');
  // Every route to a Function constructor: a plain function, async, generator and async generator.
  const kinds = [
    Object.getPrototypeOf(function () {}),
    Object.getPrototypeOf(async function () {}),
    Object.getPrototypeOf(function* () {}),
    Object.getPrototypeOf(async function* () {}),
  ];
  for (const proto of kinds) {
    lock(proto, 'constructor');
  }

  // --- the mock API -----------------------------------------------------------------------------
  const defineMock = (spec) => spec;
  let moduleValue = null;
  let sessionState = {};
  const callCounts = {};

  const fail = (id, code, message) => post({ type: 'error', id, code, message: String(message) });
  const codeOf = (error) => {
    const message = error && error.message ? String(error.message) : String(error);
    return message.startsWith('TESSERA_MOCK_SANDBOX_VIOLATION')
      ? ['TESSERA_MOCK_SANDBOX_VIOLATION', message]
      : ['TESSERA_MOCK_HANDLER_ERROR', message];
  };

  const call = (message) => {
    const { id, method, args, nowMs } = message;
    const handler = moduleValue.methods[method];
    if (typeof handler !== 'function') {
      fail(id, 'TESSERA_MOCK_METHOD_UNKNOWN', 'The mock does not define method ' + method);
      return;
    }
    const callIndex = callCounts[method] ?? 0;
    callCounts[method] = callIndex + 1;
    const ctx = objectFreeze({
      interfaceId: moduleValue.interface,
      method,
      callIndex,
      nowMs,
      random,
      get session() {
        return sessionState;
      },
    });
    try {
      const result = handler(args, ctx);
      const json = jsonStringify(result);
      if (typeof json !== 'string') {
        fail(id, 'TESSERA_MOCK_SYNTAX', 'The handler did not return JSON data');
        return;
      }
      post({ type: 'result', id, json });
    } catch (error) {
      const [code, text] = codeOf(error);
      fail(id, code, text);
    }
  };

  onMessage = (event) => {
    const message = event.data;
    if (message.type === 'call' && moduleValue !== null) {
      call(message);
    } else if (message.type === 'reset') {
      sessionState = {};
      for (const key of objectKeys(callCounts)) {
        delete callCounts[key];
      }
      seedRng();
      post({ type: 'reset-done' });
    }
  };

  return {
    defineMock,
    start(readModule) {
      try {
        const exported = readModule();
        const value = exported && exported.default;
        if (
          value === null ||
          typeof value !== 'object' ||
          typeof value.interface !== 'string' ||
          value.interface === '' ||
          value.methods === null ||
          typeof value.methods !== 'object'
        ) {
          post({
            type: 'init-error',
            code: 'TESSERA_MOCK_EXPORT_MISSING',
            message: 'The module must `export default defineMock({ interface, methods })`',
          });
          return;
        }
        moduleValue = value;
        post({ type: 'ready', interface: value.interface, methods: objectKeys(value.methods) });
      } catch (error) {
        const [code, text] = codeOf(error);
        post({ type: 'init-error', code, message: text });
      }
    },
  };
}

/**
 * The worker's script text: the prelude, then the compiled module, then the start call.
 *
 * `compiled` runs inside a function whose only free names are `defineMock` (the mock API) and whatever
 * the prelude left on the global object, which is nothing a mock can use. A syntax error in it is a
 * script error the host reports as `TESSERA_MOCK_TRANSPILE`.
 *
 * @param {string} compiled a classic script that assigns `var __mockModule = { default: ... }`
 * @param {{ seed: number }} options the session's PRNG seed
 * @returns {string}
 */
export function buildWorkerSource(compiled, options) {
  const config = JSON.stringify({ seed: options.seed >>> 0, forbidden: FORBIDDEN_IDENTIFIERS });
  return [
    '"use strict";',
    `const __api = (${String(prelude)})(${config});`,
    'const defineMock = __api.defineMock;',
    // The compiled module is a function body: its `var`s stay local and cannot touch the prelude.
    `__api.start(() => { ${compiled}\n; return typeof __mockModule === "undefined" ? undefined : __mockModule; });`,
    '',
  ].join('\n');
}
