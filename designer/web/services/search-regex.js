// SPDX-License-Identifier: MIT

// The bounded regular-expression engine of the `search` service (#580, ADR-060).
//
// Project search takes a pattern from the person and runs it over every file of the project, so a native
// `RegExp` is not an option: one pattern such as `(a+)+$` would pin the worker for minutes (and a worker
// cannot be interrupted inside a match). This engine is a Pike VM: it advances every live thread one input
// character at a time, so a match costs O(text * program) steps whatever the pattern, and a shared step
// budget ends a runaway search with a "truncated" answer instead of a hang. It supports the subset a
// code search needs and refuses the rest with a message (backreferences and lookaround need backtracking,
// which is exactly what is excluded here):
//
//   literals  .  [...] [^...]  \d \w \s \D \W \S  \b \B  ^ $  ( ) (?: )  |  * + ? {n} {n,} {n,m} and lazy forms
//   escapes   \n \r \t \f \v \0 \xHH \uHHHH \u{H...} and any escaped punctuation
//
// Matching is per line (the caller splits), `^` and `$` are line anchors, spans are UTF-16 offsets into
// the line (what CodeMirror counts) and the engine walks code points so an astral character is one unit of
// input. Like the other service engines this is JS *source text* (it runs in a worker and in Node, where
// the gate evaluates the same text), so it is a string constant and uses no template literals itself.

export const searchRegexSource = String.raw`
const RX_MAX_PATTERN = 500;
const RX_MAX_PROGRAM = 4000;
const RX_MAX_DEPTH = 40;
const RX_MAX_REPEAT = 1000;
const RX_MAX_GROUPS = 99;
const RX_DIGIT = [[48, 57]];
const RX_WORD = [[48, 57], [65, 90], [95, 95], [97, 122]];
const RX_SPACE = [[9, 13], [32, 32], [160, 160], [0x1680, 0x1680], [0x2000, 0x200a], [0x2028, 0x2029], [0x202f, 0x202f], [0x205f, 0x205f], [0x3000, 0x3000], [0xfeff, 0xfeff]];

function rxError(message) {
  const error = new Error(message);
  error.code = 'TESSERA_SEARCH_PATTERN';
  return error;
}

function rxLower(cp) {
  return String.fromCodePoint(cp).toLowerCase().codePointAt(0);
}
function rxUpper(cp) {
  return String.fromCodePoint(cp).toUpperCase().codePointAt(0);
}
function rxIsWord(code) {
  return code >= 48 && (code <= 57 || (code >= 65 && code <= 90) || code === 95 || (code >= 97 && code <= 122));
}

/** Pattern text to a syntax tree; throws a TESSERA_SEARCH_PATTERN error with a sentence for the person. */
function rxParse(source) {
  if (source.length > RX_MAX_PATTERN) throw rxError('the pattern is longer than ' + RX_MAX_PATTERN + ' characters');
  const cps = Array.from(source);
  let i = 0;
  let groups = 0;

  const isDigit = (c) => c !== undefined && c >= '0' && c <= '9';
  const hexValue = (count) => {
    const text = cps.slice(i, i + count).join('');
    if (text.length !== count || !/^[0-9a-fA-F]+$/.test(text)) throw rxError('bad hexadecimal escape');
    i += count;
    return parseInt(text, 16);
  };
  const classEscape = (c) => {
    switch (c) {
      case 'd': return { set: RX_DIGIT, neg: false };
      case 'D': return { set: RX_DIGIT, neg: true };
      case 'w': return { set: RX_WORD, neg: false };
      case 'W': return { set: RX_WORD, neg: true };
      case 's': return { set: RX_SPACE, neg: false };
      case 'S': return { set: RX_SPACE, neg: true };
      default: return null;
    }
  };
  /** The code point after a backslash (the backslash is consumed); a class escape is returned as an object. */
  const escape = (inClass) => {
    if (i >= cps.length) throw rxError('the pattern ends with a backslash');
    const c = cps[i++];
    const set = classEscape(c);
    if (set !== null) return set;
    switch (c) {
      case 'n': return 10;
      case 'r': return 13;
      case 't': return 9;
      case 'f': return 12;
      case 'v': return 11;
      case '0': return 0;
      case 'x': return hexValue(2);
      case 'u':
        if (cps[i] === '{') {
          const end = cps.indexOf('}', i);
          if (end < 0) throw rxError('bad unicode escape');
          const value = parseInt(cps.slice(i + 1, end).join(''), 16);
          if (!(value <= 0x10ffff)) throw rxError('bad unicode escape');
          i = end + 1;
          return value;
        }
        return hexValue(4);
      case 'b': return inClass ? 8 : { assert: 'wordb' };
      case 'B': if (!inClass) return { assert: 'nwordb' }; break;
      case 'k': throw rxError('named backreferences are not supported');
      default:
        if (c >= '1' && c <= '9') throw rxError('backreferences are not supported');
    }
    return c.codePointAt(0);
  };

  function parseClass() {
    const neg = cps[i] === '^';
    if (neg) i += 1;
    const items = [];
    let closed = false;
    while (i < cps.length) {
      const c = cps[i];
      if (c === ']') { i += 1; closed = true; break; }
      let lo;
      i += 1;
      lo = c === '\\' ? escape(true) : c.codePointAt(0);
      if (typeof lo === 'object') { items.push(lo); continue; }
      if (cps[i] === '-' && i + 1 < cps.length && cps[i + 1] !== ']') {
        i += 1;
        const d = cps[i++];
        const hi = d === '\\' ? escape(true) : d.codePointAt(0);
        if (typeof hi === 'object') { items.push([lo, lo], [45, 45], hi); continue; }
        if (hi < lo) throw rxError('a character class range is out of order');
        items.push([lo, hi]);
      } else {
        items.push([lo, lo]);
      }
    }
    if (!closed) throw rxError('a character class is not closed');
    return { t: 'class', neg, items };
  }

  function parseQuantifier() {
    const c = cps[i];
    let min;
    let max;
    if (c === '*') { min = 0; max = Infinity; i += 1; }
    else if (c === '+') { min = 1; max = Infinity; i += 1; }
    else if (c === '?') { min = 0; max = 1; i += 1; }
    else if (c === '{') {
      const close = cps.indexOf('}', i);
      const body = close < 0 ? '' : cps.slice(i + 1, close).join('');
      const m = /^(\d+)(,(\d*))?$/.exec(body);
      if (m === null) return null;
      min = Number(m[1]);
      max = m[2] === undefined ? min : m[3] === '' ? Infinity : Number(m[3]);
      if (max < min) throw rxError('a repeat count is out of order');
      if (min > RX_MAX_REPEAT || (max !== Infinity && max > RX_MAX_REPEAT)) throw rxError('a repeat count is larger than ' + RX_MAX_REPEAT);
      i = close + 1;
    } else {
      return null;
    }
    let greedy = true;
    if (cps[i] === '?') { greedy = false; i += 1; }
    return { min, max, greedy };
  }

  function parseAtom(depth) {
    const c = cps[i++];
    switch (c) {
      case '(': {
        if (depth >= RX_MAX_DEPTH) throw rxError('the pattern nests too deeply');
        let index = 0;
        if (cps[i] === '?') {
          if (cps[i + 1] !== ':') throw rxError('lookaround and named groups are not supported');
          i += 2;
        } else {
          if (groups >= RX_MAX_GROUPS) throw rxError('too many groups');
          index = ++groups;
        }
        const body = parseAlt(depth + 1);
        if (cps[i] !== ')') throw rxError('a group is not closed');
        i += 1;
        return { t: 'group', index, body };
      }
      case '[': return parseClass();
      case '.': return { t: 'any' };
      case '^': return { t: 'assert', kind: 'bol' };
      case '$': return { t: 'assert', kind: 'eol' };
      case '*': case '+': case '?': throw rxError('nothing to repeat');
      case '\\': {
        const e = escape(false);
        if (typeof e === 'number') return { t: 'char', cp: e };
        if (e.assert !== undefined) return { t: 'assert', kind: e.assert };
        return { t: 'class', neg: false, items: [e] };
      }
      default: return { t: 'char', cp: c.codePointAt(0) };
    }
  }

  function parseSeq(depth) {
    const items = [];
    while (i < cps.length && cps[i] !== '|' && cps[i] !== ')') {
      let atom = parseAtom(depth);
      const q = parseQuantifier();
      if (q !== null) {
        if (atom.t === 'assert') throw rxError('nothing to repeat');
        atom = { t: 'repeat', min: q.min, max: q.max, greedy: q.greedy, body: atom };
        if (cps[i] === '*' || cps[i] === '+' || cps[i] === '?') throw rxError('nothing to repeat');
      }
      items.push(atom);
    }
    return { t: 'seq', items };
  }

  function parseAlt(depth) {
    const branches = [parseSeq(depth)];
    while (cps[i] === '|') { i += 1; branches.push(parseSeq(depth)); }
    return branches.length === 1 ? branches[0] : { t: 'alt', branches };
  }

  const tree = parseAlt(0);
  if (i < cps.length) throw rxError('an unmatched )');
  return { tree, groups };
}

const RX_CHAR = 0, RX_ANY = 1, RX_CLASS = 2, RX_SPLIT = 3, RX_JMP = 4, RX_SAVE = 5, RX_ASSERT = 6, RX_MATCH = 7;

/** A syntax tree to a program (an array of instructions) with the size bound enforced while emitting. */
function rxEmit(tree, groups, ignoreCase, wholeWord) {
  const prog = [];
  const push = (inst) => {
    if (prog.length >= RX_MAX_PROGRAM) throw rxError('the pattern is too large');
    prog.push(inst);
    return prog.length - 1;
  };
  function gen(node) {
    switch (node.t) {
      case 'seq': for (const item of node.items) gen(item); return;
      case 'char': push({ op: RX_CHAR, c: ignoreCase ? rxLower(node.cp) : node.cp }); return;
      case 'any': push({ op: RX_ANY }); return;
      case 'class': push({ op: RX_CLASS, node }); return;
      case 'assert': push({ op: RX_ASSERT, kind: node.kind }); return;
      case 'group':
        if (node.index > 0) push({ op: RX_SAVE, n: node.index * 2 });
        gen(node.body);
        if (node.index > 0) push({ op: RX_SAVE, n: node.index * 2 + 1 });
        return;
      case 'alt': {
        const jumps = [];
        for (let k = 0; k < node.branches.length; k += 1) {
          if (k < node.branches.length - 1) {
            const split = push({ op: RX_SPLIT, x: 0, y: 0 });
            prog[split].x = prog.length;
            gen(node.branches[k]);
            jumps.push(push({ op: RX_JMP, x: 0 }));
            prog[split].y = prog.length;
          } else {
            gen(node.branches[k]);
          }
        }
        for (const j of jumps) prog[j].x = prog.length;
        return;
      }
      case 'repeat': {
        for (let k = 0; k < node.min; k += 1) gen(node.body);
        if (node.max === Infinity) {
          const split = push({ op: RX_SPLIT, x: 0, y: 0 });
          const body = prog.length;
          gen(node.body);
          push({ op: RX_JMP, x: split });
          const end = prog.length;
          prog[split].x = node.greedy ? body : end;
          prog[split].y = node.greedy ? end : body;
        } else {
          const splits = [];
          for (let k = node.min; k < node.max; k += 1) {
            splits.push(push({ op: RX_SPLIT, x: 0, y: 0 }));
            gen(node.body);
          }
          const end = prog.length;
          for (const s of splits) {
            prog[s].x = node.greedy ? s + 1 : end;
            prog[s].y = node.greedy ? end : s + 1;
          }
        }
        return;
      }
      default: throw rxError('internal: unknown node');
    }
  }
  if (wholeWord) push({ op: RX_ASSERT, kind: 'wl' });
  gen(tree);
  if (wholeWord) push({ op: RX_ASSERT, kind: 'wr' });
  push({ op: RX_MATCH });
  return { prog, groups, ignoreCase };
}

/** Compiles a user pattern. */
function rxCompile(source, ignoreCase, wholeWord) {
  const parsed = rxParse(source);
  return rxEmit(parsed.tree, parsed.groups, ignoreCase, wholeWord);
}

/** Compiles literal text (what the plain search falls back to when case folding changes a line's length). */
function rxCompileLiteral(text, ignoreCase, wholeWord) {
  const items = Array.from(text).map((c) => ({ t: 'char', cp: c.codePointAt(0) }));
  return rxEmit({ t: 'seq', items }, 0, ignoreCase, wholeWord);
}

function rxInSet(set, cp) {
  for (const [lo, hi] of set) if (cp >= lo && cp <= hi) return true;
  return false;
}
function rxClassTest(node, cp, ignoreCase) {
  const test = (c) => {
    for (const item of node.items) {
      if (Array.isArray(item) ? c >= item[0] && c <= item[1] : rxInSet(item.set, c) !== item.neg) return true;
    }
    return false;
  };
  let hit = test(cp);
  if (!hit && ignoreCase) hit = test(rxLower(cp)) || test(rxUpper(cp));
  return hit !== node.neg;
}

function rxAssertOk(kind, text, p) {
  const prev = p > 0 ? text.charCodeAt(p - 1) : -1;
  const next = p < text.length ? text.charCodeAt(p) : -1;
  switch (kind) {
    case 'bol': return p === 0;
    case 'eol': return p === text.length;
    case 'wordb': return rxIsWord(prev) !== rxIsWord(next);
    case 'nwordb': return rxIsWord(prev) === rxIsWord(next);
    case 'wl': return !rxIsWord(prev);
    case 'wr': return !rxIsWord(next);
    default: return false;
  }
}

/**
 * The leftmost-first match of 'program' in 'text' at or after 'from': { start, end, caps } (caps holds the
 * UTF-16 [start, end] pair of each group, -1 when unset) or null. 'budget.left' is the shared step budget;
 * running out throws an error with 'budget: true', which the caller turns into a truncated answer.
 */
function rxExec(program, text, from, budget) {
  const { prog } = program;
  const ic = program.ignoreCase;
  const size = prog.length;
  const seen = new Int32Array(size).fill(-1);
  let gen = 0;
  let clist = [];
  let nlist = [];
  const capCount = (program.groups + 1) * 2;
  const addThread = (list, pc0, p, caps0, g) => {
    const stack = [pc0, caps0];
    while (stack.length > 0) {
      const caps = stack.pop();
      const pc = stack.pop();
      if (seen[pc] === g) continue;
      seen[pc] = g;
      const inst = prog[pc];
      switch (inst.op) {
        case RX_JMP: stack.push(inst.x, caps); break;
        case RX_SPLIT: stack.push(inst.y, caps, inst.x, caps); break;
        case RX_SAVE: { const next = caps.slice(); next[inst.n] = p; stack.push(pc + 1, next); break; }
        case RX_ASSERT: if (rxAssertOk(inst.kind, text, p)) stack.push(pc + 1, caps); break;
        default: list.push({ pc, caps });
      }
    }
  };
  let matched = null;
  gen += 1;
  let p = from;
  for (;;) {
    if (matched === null) {
      const caps = new Array(capCount).fill(-1);
      caps[0] = p;
      addThread(clist, 0, p, caps, gen);
    }
    if (clist.length === 0 && (matched !== null || p >= text.length)) break;
    const cp = p < text.length ? text.codePointAt(p) : -1;
    const width = cp > 0xffff ? 2 : 1;
    gen += 1;
    for (let t = 0; t < clist.length; t += 1) {
      budget.left -= 1;
      if (budget.left < 0) { const error = new Error('step budget exhausted'); error.budget = true; throw error; }
      const { pc, caps } = clist[t];
      const inst = prog[pc];
      if (inst.op === RX_MATCH) {
        matched = caps.slice();
        matched[1] = p;
        break;
      }
      if (cp < 0) continue;
      let ok = false;
      if (inst.op === RX_CHAR) ok = (ic ? rxLower(cp) : cp) === inst.c;
      else if (inst.op === RX_ANY) ok = cp !== 10 && cp !== 13 && cp !== 0x2028 && cp !== 0x2029;
      else if (inst.op === RX_CLASS) ok = rxClassTest(inst.node, cp, ic);
      if (ok) addThread(nlist, pc + 1, p + width, caps, gen);
    }
    const swap = clist;
    clist = nlist;
    nlist = swap;
    nlist.length = 0;
    if (p >= text.length) {
      // The end of the line: whatever is left in the list can only be MATCH threads, handled next round.
      if (clist.length === 0) break;
      p += 1;
      continue;
    }
    p += width;
  }
  return matched === null ? null : { start: matched[0], end: matched[1], caps: matched };
}
`;
