// SPDX-License-Identifier: MIT

// The matcher of the `search` service (#580, ADR-060): globs, the compiled query, the per-file walk, the
// search and the replace plan. Like `search-regex.js` it is JS source text (a worker and the Node gate run
// the same text), and it is appended after that engine, whose `rx*` functions it calls. No template
// literals inside, so the constant can be a single raw string.
//
// Spans are UTF-16 offsets into the whole file (what CodeMirror counts) and `line` and `column` are
// 1-based. Every limit is a constant here so the gate can name it: results, files, a file's size, a line's
// length, the step budget and the wall-clock budget each end the walk with a reason instead of a hang.

export const searchEngineSource = String.raw`
const SEARCH_MAX_RESULTS = 2000;
const SEARCH_HARD_RESULTS = 50000;
const SEARCH_MAX_FILE_CHARS = 1000000;
const SEARCH_MAX_LINE_CHARS = 20000;
const SEARCH_MAX_QUERY = 1000;
const SEARCH_STEP_BUDGET = 40000000;
const SEARCH_TIME_BUDGET_MS = 4000;
const SEARCH_PREVIEW_CHARS = 160;

function searchError(code, message) {
  const error = new Error(message);
  error.code = code;
  return error;
}

/** One glob to a RegExp over a project path: * stays in a segment, ** crosses them, ? is one character, {a,b} alternates. */
function searchGlobToRegExp(glob) {
  let out = '';
  let depth = 0;
  for (let i = 0; i < glob.length; i += 1) {
    const c = glob[i];
    if (c === '*') {
      if (glob[i + 1] === '*') {
        i += 1;
        if (glob[i + 1] === '/') { i += 1; out += '(?:.*/)?'; } else out += '.*';
      } else out += '[^/]*';
    } else if (c === '?') out += '[^/]';
    else if (c === '{') { depth += 1; out += '(?:'; }
    else if (c === '}' && depth > 0) { depth -= 1; out += ')'; }
    else if (c === ',' && depth > 0) out += '|';
    else out += c.replace(/[.+^$()|[\]\\{}]/g, '\\$&');
  }
  while (depth > 0) { depth -= 1; out += ')'; }
  return new RegExp('^' + out + '$');
}

/**
 * A comma-separated glob list to a predicate over project paths. A pattern without a slash matches at any
 * depth (the way an editor reads *.tsx), and a pattern that names a folder also matches everything below it,
 * so 'dist' and 'dist/' both exclude the folder.
 */
function searchGlobList(text) {
  // Commas inside braces belong to the glob ({css,json}); only the others separate patterns.
  const parts = [];
  let current = '';
  let depth = 0;
  for (const c of String(text || '')) {
    if (c === '{') depth += 1;
    else if (c === '}' && depth > 0) depth -= 1;
    if (c === ',' && depth === 0) { parts.push(current); current = ''; } else current += c;
  }
  parts.push(current);
  const globs = parts.map((part) => part.trim()).filter((part) => part !== '');
  if (globs.length === 0) return null;
  const tests = [];
  for (let part of globs) {
    if (part.length > 300) throw searchError('TESSERA_SEARCH_GLOB', 'a glob is longer than 300 characters');
    if (part.startsWith('./')) part = part.slice(2);
    if (part.startsWith('/')) part = part.slice(1);
    if (part.endsWith('/')) part = part.slice(0, -1);
    const rooted = part.includes('/') ? part : '**/' + part;
    tests.push(searchGlobToRegExp(rooted), searchGlobToRegExp(rooted + '/**'));
  }
  return (path) => tests.some((test) => test.test(path));
}

/** The options to a matcher: { next(line, from, budget) -> { start, end, caps } | null, expand(match, replacement) }. */
function searchCompile(options) {
  const query = typeof options.query === 'string' ? options.query : '';
  if (query === '') throw searchError('TESSERA_SEARCH_PATTERN', 'the search text is empty');
  if (query.length > SEARCH_MAX_QUERY) throw searchError('TESSERA_SEARCH_PATTERN', 'the search text is longer than ' + SEARCH_MAX_QUERY + ' characters');
  const caseSensitive = options.caseSensitive === true;
  const whole = options.wholeWord === true;
  if (options.regex === true) {
    const program = rxCompile(query, !caseSensitive, whole);
    return {
      regex: true,
      next: (line, from, budget) => rxExec(program, line, from, budget),
      expand: (match, line, replacement) => searchExpand(match, line, replacement),
    };
  }
  const needle = caseSensitive ? query : query.toLowerCase();
  const first = query.charCodeAt(0);
  const last = query.charCodeAt(query.length - 1);
  const checkLeft = rxIsWord(first);
  const checkRight = rxIsWord(last);
  let program = null;
  const boundaryOk = (line, start, end) =>
    !whole || ((!checkLeft || start === 0 || !rxIsWord(line.charCodeAt(start - 1))) &&
      (!checkRight || end >= line.length || !rxIsWord(line.charCodeAt(end))));
  return {
    regex: false,
    next(line, from, budget) {
      const haystack = caseSensitive ? line : line.toLowerCase();
      if (haystack.length !== line.length || (!caseSensitive && needle.length !== query.length)) {
        // Case folding changed a length (a dotted capital I, say), so offsets would drift: let the VM walk the line.
        program ??= rxCompileLiteral(query, !caseSensitive, whole);
        return rxExec(program, line, from, budget);
      }
      let at = haystack.indexOf(needle, from);
      while (at >= 0) {
        budget.left -= 1;
        if (budget.left < 0) { const error = new Error('step budget exhausted'); error.budget = true; throw error; }
        if (boundaryOk(line, at, at + needle.length)) return { start: at, end: at + needle.length, caps: [at, at + needle.length] };
        at = haystack.indexOf(needle, at + 1);
      }
      return null;
    },
    expand: (match, line, replacement) => replacement,
  };
}

/** The replacement text for a regex match: $& the match, $1..$99 a group, $$ a dollar sign. */
function searchExpand(match, line, replacement) {
  let out = '';
  for (let i = 0; i < replacement.length; i += 1) {
    const c = replacement[i];
    if (c !== '$' || i + 1 >= replacement.length) { out += c; continue; }
    const d = replacement[i + 1];
    if (d === '$') { out += '$'; i += 1; continue; }
    if (d === '&') { out += line.slice(match.start, match.end); i += 1; continue; }
    if (d >= '0' && d <= '9') {
      const groups = match.caps.length / 2 - 1;
      let digits = d;
      if (replacement[i + 2] >= '0' && replacement[i + 2] <= '9' && Number(d + replacement[i + 2]) <= groups) digits = d + replacement[i + 2];
      const n = Number(digits);
      if (n >= 1 && n <= groups) {
        const s = match.caps[n * 2];
        const e = match.caps[n * 2 + 1];
        if (s >= 0 && e >= 0) out += line.slice(s, e);
        i += digits.length;
        continue;
      }
    }
    out += c;
  }
  return out;
}

function searchIsBinary(text) {
  const head = text.length > 8000 ? text.slice(0, 8000) : text;
  return head.includes(String.fromCharCode(0));
}

const searchTick = () => new Promise((resolve) => setTimeout(resolve, 0));

/**
 * Walks every file the globs admit, calling 'visit(file, text, lineStart, line, lineNumber, match)' for each
 * non-empty match in order; 'visit' returns false to stop. Yields to the event loop between files
 * so a cancel message is seen. Returns the walk's tally.
 */
async function searchWalk(files, options, visit, shouldStop) {
  const matcher = searchCompile(options);
  const include = searchGlobList(options.include);
  const exclude = searchGlobList(options.exclude);
  const budget = { left: Math.min(SEARCH_STEP_BUDGET, Number.isInteger(options.stepBudget) ? options.stepBudget : SEARCH_STEP_BUDGET) };
  const started = Date.now();
  let lastYield = started;
  const tally = { searched: 0, skippedBinary: 0, skippedLarge: 0, longLines: 0, stopped: null };
  const ordered = files.slice().sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
  for (const file of ordered) {
    if (include !== null && !include(file.path)) continue;
    if (exclude !== null && exclude(file.path)) continue;
    const text = file.text;
    if (text.length > SEARCH_MAX_FILE_CHARS) { tally.skippedLarge += 1; continue; }
    if (searchIsBinary(text)) { tally.skippedBinary += 1; continue; }
    tally.searched += 1;
    if (Date.now() - lastYield > 8) {
      await searchTick();
      lastYield = Date.now();
      if (shouldStop()) { tally.stopped = 'cancelled'; return tally; }
    }
    if (Date.now() - started > SEARCH_TIME_BUDGET_MS) { tally.stopped = 'time'; return tally; }
    let lineStart = 0;
    let lineNumber = 0;
    while (lineStart <= text.length) {
      let lineEnd = text.indexOf('\n', lineStart);
      if (lineEnd < 0) lineEnd = text.length;
      lineNumber += 1;
      let line = text.slice(lineStart, lineEnd);
      if (line.endsWith('\r')) line = line.slice(0, -1);
      if (line.length > SEARCH_MAX_LINE_CHARS) {
        tally.longLines += 1;
      } else if (line.length > 0) {
        let from = 0;
        while (from <= line.length) {
          let match;
          try { match = matcher.next(line, from, budget); } catch (error) {
            if (error.budget === true) { tally.stopped = 'budget'; return tally; }
            throw error;
          }
          if (match === null) break;
          if (match.end === match.start) { from = match.end + 1; continue; }
          if (visit(file, text, lineStart, line, lineNumber, match, matcher) === false) { tally.stopped = 'results'; return tally; }
          from = match.end;
        }
      }
      lineStart = lineEnd + 1;
    }
  }
  return tally;
}

/** A line shortened around a match for the results list: leading space trimmed, long lines windowed. */
function searchPreview(line, start, end) {
  let from = 0;
  while (from < start && (line[from] === ' ' || line[from] === String.fromCharCode(9))) from += 1;
  let text = line.slice(from);
  let at = start - from;
  const room = SEARCH_PREVIEW_CHARS;
  if (at > room / 2) {
    const cut = at - Math.floor(room / 4);
    text = text.slice(cut);
    at -= cut;
  }
  const length = Math.min(end - start, room);
  if (text.length > room + length) text = text.slice(0, room + length);
  return { text, start: at, length };
}

/** The search: matches grouped by file, capped, with the tally of what was skipped and why a walk stopped early. */
async function tesseraSearch(files, options, shouldStop) {
  const cap = Math.min(Number.isInteger(options.maxResults) && options.maxResults > 0 ? options.maxResults : SEARCH_MAX_RESULTS, SEARCH_HARD_RESULTS);
  const groups = new Map();
  let total = 0;
  const tally = await searchWalk(files, options, (file, text, lineStart, line, lineNumber, match) => {
    if (total >= cap) return false;
    let group = groups.get(file.file);
    if (group === undefined) {
      group = { file: file.file, path: file.path, matches: [] };
      groups.set(file.file, group);
    }
    const preview = searchPreview(line, match.start, match.end);
    group.matches.push({
      offset: lineStart + match.start,
      length: match.end - match.start,
      line: lineNumber,
      column: match.start + 1,
      text: preview.text,
      start: preview.start,
      end: preview.start + preview.length,
    });
    total += 1;
    return true;
  }, shouldStop);
  const truncated = tally.stopped === 'results' ? 'results' : tally.stopped;
  return {
    files: [...groups.values()],
    total,
    truncated,
    searched: tally.searched,
    skippedBinary: tally.skippedBinary,
    skippedLarge: tally.skippedLarge,
    longLines: tally.longLines,
  };
}

/**
 * The replace plan: 'only' is null (every match) or a list of { file, offset } picks. The edits use offsets
 * into the snapshot's text, ascending and non-overlapping per file; the page applies them as one operation.
 */
async function tesseraReplace(files, options, replacement, only, shouldStop) {
  const picks = only === null ? null : new Set(only.map((pick) => pick.file + ':' + pick.offset));
  const edits = [];
  const touched = new Set();
  const tally = await searchWalk(files, options, (file, text, lineStart, line, lineNumber, match, matcher) => {
    if (edits.length >= SEARCH_HARD_RESULTS) return false;
    const offset = lineStart + match.start;
    if (picks !== null && !picks.has(file.file + ':' + offset)) return true;
    edits.push({ file: file.file, offset, length: match.end - match.start, newText: matcher.expand(match, line, replacement) });
    touched.add(file.file);
    return true;
  }, shouldStop);
  return { edits, files: touched.size, truncated: tally.stopped };
}
`;
