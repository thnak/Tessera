// SPDX-License-Identifier: MIT

// The when-clause evaluator (`studio-workbench-v1.md` section 2.2).
//
// A deliberately tiny grammar so a Tessera-built UI can evaluate the same text: identifiers, string and
// number literals, `true`/`false`, `== != && || !` and parentheses. No calls, no arithmetic. A clause is a
// pure function of a flat context record; an unknown key reads as `false` and evaluation never throws, so
// a stale clause degrades to "disabled" instead of breaking a menu.
//
//   expr    := or
//   or      := and ( '||' and )*
//   and     := cmp ( '&&' cmp )*
//   cmp     := unary ( ( '==' | '!=' ) unary )?
//   unary   := '!' unary | primary
//   primary := '(' expr ')' | identifier | string | number | 'true' | 'false'
//
// Pure ES module, no DOM and no framework import (the workbench-core rule).

const MAX_CLAUSE_LENGTH = 400;

/** @typedef {{ t: string, v?: unknown }} Token */

/** @param {string} source @returns {Token[]} */
function tokenize(source) {
  const tokens = [];
  let i = 0;
  while (i < source.length) {
    const c = source[i];
    if (c === " " || c === "\t") {
      i += 1;
    } else if (c === "(" || c === ")") {
      tokens.push({ t: c });
      i += 1;
    } else if (source.startsWith("&&", i) || source.startsWith("||", i) || source.startsWith("==", i) || source.startsWith("!=", i)) {
      tokens.push({ t: source.slice(i, i + 2) });
      i += 2;
    } else if (c === "!") {
      tokens.push({ t: "!" });
      i += 1;
    } else if (c === "'" || c === '"') {
      const end = source.indexOf(c, i + 1);
      if (end < 0) {
        throw new SyntaxError(`unterminated string at ${i}`);
      }
      tokens.push({ t: "lit", v: source.slice(i + 1, end) });
      i = end + 1;
    } else if (/[0-9]/.test(c)) {
      let j = i;
      while (j < source.length && /[0-9.]/.test(source[j])) {
        j += 1;
      }
      tokens.push({ t: "lit", v: Number(source.slice(i, j)) });
      i = j;
    } else if (/[A-Za-z_]/.test(c)) {
      let j = i;
      while (j < source.length && /[A-Za-z0-9_.-]/.test(source[j])) {
        j += 1;
      }
      const word = source.slice(i, j);
      if (word === "true" || word === "false") {
        tokens.push({ t: "lit", v: word === "true" });
      } else {
        tokens.push({ t: "id", v: word });
      }
      i = j;
    } else {
      throw new SyntaxError(`unexpected character '${c}' at ${i}`);
    }
  }
  return tokens;
}

/** @param {Token[]} tokens */
function parseTokens(tokens) {
  let pos = 0;
  const peek = () => tokens[pos];
  const take = () => tokens[pos++];

  function primary() {
    const token = take();
    if (token === undefined) {
      throw new SyntaxError("unexpected end of clause");
    }
    if (token.t === "(") {
      const inner = or();
      if (take()?.t !== ")") {
        throw new SyntaxError("missing )");
      }
      return inner;
    }
    if (token.t === "lit") {
      return { k: "lit", v: token.v };
    }
    if (token.t === "id") {
      return { k: "id", v: token.v };
    }
    throw new SyntaxError(`unexpected '${token.t}'`);
  }
  function unary() {
    if (peek()?.t === "!") {
      take();
      return { k: "not", a: unary() };
    }
    return primary();
  }
  function cmp() {
    const left = unary();
    const op = peek()?.t;
    if (op === "==" || op === "!=") {
      take();
      return { k: op, a: left, b: unary() };
    }
    return left;
  }
  function and() {
    let left = cmp();
    while (peek()?.t === "&&") {
      take();
      left = { k: "&&", a: left, b: cmp() };
    }
    return left;
  }
  function or() {
    let left = and();
    while (peek()?.t === "||") {
      take();
      left = { k: "||", a: left, b: and() };
    }
    return left;
  }
  const ast = or();
  if (pos !== tokens.length) {
    throw new SyntaxError(`unexpected '${tokens[pos].t}'`);
  }
  return ast;
}

function leaves(node) {
  switch (node.k) {
    case "&&":
    case "||":
      return leaves(node.a) + leaves(node.b);
    case "not":
      return leaves(node.a);
    case "==":
    case "!=":
      return 1;
    default:
      return 1;
  }
}

function identifiers(node, into) {
  if (node.k === "id") {
    into.add(node.v);
  } else if (node.a !== undefined) {
    identifiers(node.a, into);
    if (node.b !== undefined) {
      identifiers(node.b, into);
    }
  }
  return into;
}

const cache = new Map();

/**
 * Parses a clause. `{ ok: true, ast, specificity, keys }` or `{ ok: false, error }`. Cached by text.
 * An empty or null clause is not a clause: callers decide what absence means.
 * @param {string} source
 */
export function parseClause(source) {
  const hit = cache.get(source);
  if (hit !== undefined) {
    return hit;
  }
  let result;
  try {
    if (typeof source !== "string" || source.trim() === "") {
      throw new SyntaxError("empty clause");
    }
    if (source.length > MAX_CLAUSE_LENGTH) {
      throw new SyntaxError("clause too long");
    }
    const ast = parseTokens(tokenize(source));
    result = { ok: true, ast, specificity: leaves(ast), keys: [...identifiers(ast, new Set())] };
  } catch (error) {
    result = { ok: false, error: error instanceof Error ? error.message : String(error) };
  }
  if (cache.size > 500) {
    cache.clear();
  }
  cache.set(source, result);
  return result;
}

/** The error text of a clause, or `null` when it parses. */
export function clauseError(source) {
  const parsed = parseClause(source);
  return parsed.ok ? null : parsed.error;
}

function value(node, context) {
  if (node.k === "lit") {
    return node.v;
  }
  if (node.k === "id") {
    return Object.hasOwn(context, node.v) ? context[node.v] : false;
  }
  return run(node, context);
}

function run(node, context) {
  switch (node.k) {
    case "&&":
      return Boolean(run(node.a, context)) && Boolean(run(node.b, context));
    case "||":
      return Boolean(run(node.a, context)) || Boolean(run(node.b, context));
    case "not":
      return !run(node.a, context);
    case "==":
      return value(node.a, context) === value(node.b, context);
    case "!=":
      return value(node.a, context) !== value(node.b, context);
    default:
      return Boolean(value(node, context));
  }
}

/**
 * Evaluates `source` against a flat context record. Never throws: a malformed clause is `false`.
 * @param {string} source
 * @param {Record<string, unknown>} context
 */
export function evaluate(source, context) {
  const parsed = parseClause(source);
  if (!parsed.ok) {
    return false;
  }
  return run(parsed.ast, context);
}

/** How specific a clause is: the number of tests it makes. `0` for no clause. Used to rank bindings. */
export function specificityOf(source) {
  if (source === null || source === undefined || source === "") {
    return 0;
  }
  const parsed = parseClause(source);
  return parsed.ok ? parsed.specificity : 0;
}

/** The context keys a clause reads (for the vocabulary check). */
export function clauseKeys(source) {
  const parsed = parseClause(source);
  return parsed.ok ? parsed.keys : [];
}
