// SPDX-License-Identifier: MIT

// Reading and writing a project's theme tokens as text edits (issue #582).
//
// Two formats, one model. A **theme asset** (`*.theme.json`, docs/specs/stylesheet-v1.md 9.1, what
// `tessera theme --seed` writes) holds `themes: { <id>: { "--token": "value" } }`; a **stylesheet** holds
// custom properties in `:root` blocks. Both are read into variants of tokens that remember where each value
// sits in the source, and a write replaces exactly that span. Nothing is re-serialized, so comments, order,
// indentation and every byte outside the value survive; the edit is an ordinary source edit that format, diff
// and undo already understand.
//
// A plain module so the page and the Node gate import the same bytes.

/** @typedef {{ name: string, value: string, from: number, to: number }} TokenSpan */
/** @typedef {{ id: string, tokens: TokenSpan[] }} Variant */

/** The format a path is edited as, or null when the studio does not treat it as a token file. */
export function formatOf(path) {
  if (/(^|\/)[^/]*theme[^/]*\.json$/i.test(path)) {
    return 'json';
  }
  return /\.css$/i.test(path) ? 'css' : null;
}

// ---- JSON: a scanner that keeps offsets --------------------------------------------------------------------

function skipSpace(text, at) {
  while (at < text.length && /\s/.test(text[at])) {
    at += 1;
  }
  return at;
}

/** Parses one JSON value starting at `at`; objects keep their members' spans. Throws on malformed input. */
function scanValue(text, at) {
  at = skipSpace(text, at);
  const ch = text[at];
  if (ch === '{') {
    const members = [];
    let cursor = skipSpace(text, at + 1);
    if (text[cursor] === '}') {
      return { type: 'object', from: at, to: cursor + 1, members };
    }
    for (;;) {
      const key = scanValue(text, cursor);
      if (key.type !== 'string') {
        throw new Error('object key is not a string');
      }
      cursor = skipSpace(text, key.to);
      if (text[cursor] !== ':') {
        throw new Error('expected ":"');
      }
      const value = scanValue(text, cursor + 1);
      members.push({ key: JSON.parse(text.slice(key.from, key.to)), value });
      cursor = skipSpace(text, value.to);
      if (text[cursor] === ',') {
        cursor = skipSpace(text, cursor + 1);
        continue;
      }
      if (text[cursor] === '}') {
        return { type: 'object', from: at, to: cursor + 1, members };
      }
      throw new Error('expected "," or "}"');
    }
  }
  if (ch === '[') {
    let cursor = skipSpace(text, at + 1);
    if (text[cursor] === ']') {
      return { type: 'array', from: at, to: cursor + 1 };
    }
    for (;;) {
      const item = scanValue(text, cursor);
      cursor = skipSpace(text, item.to);
      if (text[cursor] === ',') {
        cursor = skipSpace(text, cursor + 1);
        continue;
      }
      if (text[cursor] === ']') {
        return { type: 'array', from: at, to: cursor + 1 };
      }
      throw new Error('expected "," or "]"');
    }
  }
  if (ch === '"') {
    let cursor = at + 1;
    while (cursor < text.length && text[cursor] !== '"') {
      cursor += text[cursor] === '\\' ? 2 : 1;
    }
    if (cursor >= text.length) {
      throw new Error('unterminated string');
    }
    return { type: 'string', from: at, to: cursor + 1 };
  }
  const literal = /^(?:-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null)/.exec(text.slice(at, at + 40));
  if (literal === null) {
    throw new Error('unexpected character at ' + at);
  }
  return { type: 'literal', from: at, to: at + literal[0].length };
}

function readJson(text) {
  let root;
  try {
    root = scanValue(text, 0);
  } catch (error) {
    return { ok: false, message: 'Not valid JSON: ' + error.message };
  }
  const themes = root.type === 'object' ? root.members.find((member) => member.key === 'themes') : undefined;
  if (themes === undefined || themes.value.type !== 'object') {
    return { ok: false, message: 'A theme asset needs a "themes" object.' };
  }
  const variants = [];
  for (const theme of themes.value.members) {
    if (theme.value.type !== 'object') {
      continue;
    }
    const tokens = [];
    for (const member of theme.value.members) {
      if (member.key.startsWith('--') && member.value.type === 'string') {
        tokens.push({ name: member.key, value: JSON.parse(text.slice(member.value.from, member.value.to)), from: member.value.from, to: member.value.to });
      }
    }
    variants.push({ id: theme.key, tokens });
  }
  return { ok: true, format: 'json', variants };
}

// ---- CSS: custom properties with their block context -------------------------------------------------------

/** The variant a rule belongs to, from its selector and the @media conditions around it; null when it is not a token block. */
function variantOfRule(selector, media) {
  const scheme = /data-theme\s*=\s*["']?([a-z][a-z0-9-]*)["']?/.exec(selector);
  if (scheme !== null) {
    return scheme[1];
  }
  if (!/:root|(^|[\s,])html\b/.test(selector)) {
    return null;
  }
  if (/prefers-contrast\s*:\s*more/.test(media)) {
    return 'high-contrast';
  }
  if (/prefers-color-scheme\s*:\s*dark/.test(media)) {
    return 'dark';
  }
  return 'light';
}

function readCss(text) {
  const byId = new Map();
  const media = [];
  let cursor = 0;
  let preludeFrom = 0;
  /** Which open braces are @media, so a closing brace pops the right context. */
  const stack = [];
  while (cursor < text.length) {
    const ch = text[cursor];
    if (ch === '/' && text[cursor + 1] === '*') {
      const end = text.indexOf('*/', cursor + 2);
      cursor = end < 0 ? text.length : end + 2;
      if (stack.length === 0 || stack[stack.length - 1] !== 'rule') {
        preludeFrom = cursor;
      }
      continue;
    }
    if (ch === '"' || ch === "'") {
      let end = cursor + 1;
      while (end < text.length && text[end] !== ch) {
        end += text[end] === '\\' ? 2 : 1;
      }
      cursor = end + 1;
      continue;
    }
    if (ch === '{') {
      const prelude = text.slice(preludeFrom, cursor).replace(/\/\*[\s\S]*?\*\//g, '').trim();
      if (prelude.startsWith('@')) {
        stack.push('at');
        media.push(prelude);
        cursor += 1;
        preludeFrom = cursor;
        continue;
      }
      const id = variantOfRule(prelude, media.join(' '));
      const close = findBlockEnd(text, cursor + 1);
      if (id !== null) {
        const tokens = byId.get(id) ?? [];
        byId.set(id, tokens);
        collectDeclarations(text, cursor + 1, close, tokens);
      }
      cursor = close + 1;
      preludeFrom = cursor;
      continue;
    }
    if (ch === '}') {
      if (stack.pop() === 'at') {
        media.pop();
      }
      cursor += 1;
      preludeFrom = cursor;
      continue;
    }
    cursor += 1;
  }
  return { ok: true, format: 'css', variants: [...byId].map(([id, tokens]) => ({ id, tokens })) };
}

/** The offset of the `}` closing a block whose body starts at `from`. */
function findBlockEnd(text, from) {
  let depth = 1;
  for (let at = from; at < text.length; at += 1) {
    if (text[at] === '/' && text[at + 1] === '*') {
      const end = text.indexOf('*/', at + 2);
      at = end < 0 ? text.length : end + 1;
    } else if (text[at] === '{') {
      depth += 1;
    } else if (text[at] === '}') {
      depth -= 1;
      if (depth === 0) {
        return at;
      }
    }
  }
  return text.length;
}

function collectDeclarations(text, from, to, out) {
  const body = text.slice(from, to).replace(/\/\*[\s\S]*?\*\//g, (comment) => ' '.repeat(comment.length));
  const pattern = /(--[A-Za-z0-9_-]+)\s*:\s*([^;{}]*?)\s*(?=;|$)/g;
  for (const match of body.matchAll(pattern)) {
    const value = match[2];
    if (value === '') {
      continue;
    }
    const start = from + match.index + match[0].lastIndexOf(value);
    out.push({ name: match[1], value: text.slice(start, start + value.length), from: start, to: start + value.length });
  }
}

/**
 * Reads the token variants of a file.
 *
 * @param {string} text
 * @param {'json' | 'css'} format
 * @returns {{ ok: true, format: string, variants: Variant[] } | { ok: false, message: string }}
 */
export function readTokens(text, format) {
  return format === 'json' ? readJson(text) : readCss(text);
}

/** Whether a token value is a plain color the editor can offer a picker for. */
export function isColorValue(value) {
  return /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.test(value.trim()) || /^(rgba?|hsla?)\(/i.test(value.trim());
}

/**
 * Replaces the value of one token in one variant. The new value must be one declaration's worth of text, and a
 * color token stays a color, so a typo cannot silently turn a role into something the engine drops.
 *
 * @returns {{ ok: true, text: string, previous: string } | { ok: false, message: string }}
 */
export function setToken(text, format, variantId, name, value) {
  const read = readTokens(text, format);
  if (!read.ok) {
    return read;
  }
  const variant = read.variants.find((candidate) => candidate.id === variantId);
  const span = variant?.tokens.find((token) => token.name === name);
  if (span === undefined) {
    return { ok: false, message: name + ' is not defined in ' + variantId + '.' };
  }
  const next = value.trim();
  if (next === '' || /[;{}\n\r]/.test(next)) {
    return { ok: false, message: 'A token value is one non-empty declaration value.' };
  }
  if (isColorValue(span.value) && !isColorValue(next)) {
    return { ok: false, message: name + ' is a color; "' + next + '" is not.' };
  }
  const written = format === 'json' ? JSON.stringify(next) : next;
  return { ok: true, previous: span.value, text: text.slice(0, span.from) + written + text.slice(span.to) };
}
