# Source format v1: the compiler-owned formatter

- Status: slices 1, 2 and 3 implemented (`.tsx` whole file, CLI, edit list, the studio format worker and
  Format Document; `.css` and `tessera.json`, sections 12 and 13). Slice 4 of
  [ADR-061](../../decisions/ADR-061-compiler-owned-formatter.md) (range formatting) is not in this version
  and is listed in section 10.
- Normative for: what `format_source` and `tessera fmt` guarantee, the comment attachment rules and
  the style table. The grammar being printed is
  [`jsx-subset-v1.md`](jsx-subset-v1.md) and [`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md).
- Code: `src/compiler/format/` (library `tessera_source_format`), `tools/fmt/` (the CLI and the Wasm
  entry), `designer/web/services/format-service.js` (the worker), tests in
  `tests/unit/source_format_*.cpp` and `scripts/ci/preview_format_service.mjs`.

## 1. Why a token printer

The formatter prints from the **token stream**, not from the AST. The AST drops parentheses,
desugars `useMemo`/`useCallback`, expands intersection types, skips helper bodies and keeps only
decoded literal values, so a printer built on it could not promise that the output means what the
input meant. The scanner (`scan.cpp`) drives the parser's own lexer through the same modes the
parser uses (code, tag, JSX child, template substitution), so token boundaries are the parser's.
The parser is still the authority on what a file *is*: a file is parsed first and refused if it
does not parse (section 7).

## 2. API

```cpp
FormatResult format_source(FileKind kind, std::string_view text, const FormatOptions& options = {});
```

- `FileKind::Tsx`, `Css` (any `.css`) and `Json` (a file named `tessera.json`); `file_kind_for_path` maps a
  path to a kind and returns nothing for any other file.
- `FormatOptions`: `print_width` (default 100) and `indent_width` (default 2). These are the two
  options ADR-061 allows; `format_options_from_tessera_json` reads them from `format.printWidth` and
  `format.indentWidth` (section 13).
- `FormatResult`: `text` (empty when refused), `changed`, and `diagnostics`. A result is either
  complete output or a refusal, never partial output.
- Line endings follow the input: a file that contains CRLF is printed with CRLF, anything else with
  LF. A leading UTF-8 byte order mark is kept.

## 3. Properties (each is enforced on every call and tested)

1. **Idempotent:** `format(format(x)) == format(x)`. Tested over every corpus file.
2. **Meaning-preserving:** after printing, the output is parsed again and `dump(Module)` must equal
   the dump of the input (spans are not part of the dump). A mismatch is a refusal
   (`TESSERA_FMT_VERIFY`), so a printer bug cannot reach a file.
3. **No comment lost:** the output's comment sequence must equal the input's (compared ignoring
   whitespace, because a JSDoc-shaped block is re-indented). Same refusal.
4. **Deterministic:** no locale, clock, platform or iteration-order input; widths count code points.
5. **Refuses what it cannot parse** (section 7).

## 4. Comment attachment (a permanent contract)

The lexer records every comment with its span. Each comment is attached by position, before
printing:

| Comment | Attaches to | Printed |
| --- | --- | --- |
| On its own line | the next token | on its own line(s) before it, at its indent; blank lines before it are kept (at most one) |
| After code on the same line | the previous token | after that token, on the same line; a `//` comment is held to the end of the printed line |
| Inside an empty construct (`{/* c */}`, `foo(/* c */)`, `{ // c }`) | the construct (dangling) | inline for a block comment, one per line for the rest |
| Own line before a closing bracket | the closing bracket | last, one per line, inside the group |

A leading comment is only placeable where the printer may start a new line: before a statement, a
list or object item, a JSX attribute, or a closing bracket. Two cases cannot be placed: a comment that
leads any other token (for example an own-line comment between the operands of `a + b`), and a
comment inside a template-literal substitution, which is printed verbatim. The file is then **refused** with
`TESSERA_FMT_COMMENT` and the message names the comment and its line. A comment is never dropped,
moved to another construct, or guessed.

A block comment whose continuation lines all start with `*` is re-indented under its first line with
one space before each `*`; any other multi-line block is printed byte for byte. A `// Note: bind::<id>` annotation is
an ordinary own-line comment, so it keeps its line directly above the statement it annotates; a blank
line between them in the source is a blank line in the output, because the annotation pairing
(`jsx-subset-v1.md` section 9.3) is by adjacency.

## 5. Style table

| Topic | Rule |
| --- | --- |
| Indent, width | 2 spaces, 100 columns (`FormatOptions`) |
| Strings | double quotes; a single-quoted string is converted unless it contains `"` or a backslash |
| Semicolons | kept where written; none added or removed (the subset makes them optional after blocks) |
| Spacing | one space around binary operators, `=>`, `?`, `:` of a conditional, after `,`; none before `,` `;` `.` or inside `()` `[]`; `{ a, b }` for objects, imports, type objects |
| Type arguments | `Foo<int>`, no inner spaces; a `<` glued to a name with type-like tokens up to a matching `>` is a type-argument list, every other `<` `>` is a comparison with spaces |
| Brackets that break | one item per line; trailing comma for arrays and objects, `;` for type members and block-style objects; none in call arguments or parameters |
| Calls | the last argument hugs when it starts an object, array or arrow (`f(a, () => {`); a first-argument arrow with a block hugs too (`useEffect(() => {...}, [deps])`) |
| Arrow body | an expression body that does not fit moves under the `=>`; a block, a parenthesised body and a JSX body stay on the `=>` line |
| Logical chains | `&&` `||` `??` break after the operator, continuation indented; not applied around JSX |
| Blocks | `{`, one statement per line, `}`; `else` stays on the `}` line; `case`/`default` bodies indent one level |
| Blank lines | at most one, kept between statements, items of a broken list, and JSX children; none at the start or end of a block or at the start of the file |
| JSX tag | `<A b="c" />` flat; one attribute per line when it breaks, `>` or `/>` on its own line; a tag with a single attribute never breaks; attributes are never reordered (`className` and `style` keep the author's order) |
| JSX element | flat if it fits and has at most one node child or any prose; two or more element/`{}` children and no prose always break, one child per line |
| JSX parentheses | a multi-line element after `return`, `=>`, `=`, `?`, `:`, `&&` is wrapped in `(` `)`, a flat one is not |
| Template literals | printed verbatim |
| Not formatted | ternary chains and binary operators other than `&&` `||` `??` do not break: an over-long one stays on its line |

## 6. JSX text and whitespace

Whitespace in JSX text is meaning. The printer breaks only where JSX itself drops the whitespace:

- between elements and `{}` containers (a newline there is dropped by JSX);
- between the words of a text run (a newline there collapses to the single space it replaces); runs
  are filled to the width;
- leading or trailing whitespace of a text run that holds **no newline** is significant, so it is kept
  verbatim and glued to its neighbour, and no line break is placed beside it;
- a run of several spaces inside one line is text and is kept.

## 7. Refusals

| Code | When |
| --- | --- |
| `TESSERA_FMT_SYNTAX` | the parser reports an error; the parser's diagnostics follow in the message |
| `TESSERA_FMT_COMMENT` | a comment cannot be placed (section 4) |
| `TESSERA_FMT_UNSUPPORTED` | the file kind is not formatted, or the scanner cannot read the structure |
| `TESSERA_FMT_VERIFY` | the printed output does not parse, or differs in meaning or comments: a formatter bug, reported instead of written |

A refusal carries line and column and no text. A file with recoverable errors is not formatted in
slice 1 (range formatting is slice 4).

## 8. Command line

```text
tessera_fmt [--check] [paths...]      # `tessera fmt` once ADR-077's unified command exists
```

A path is a file or a directory (walked for `.tsx`, `.css` and `tessera.json`, skipping `node_modules` and
dot directories; a file named on the command line is formatted by its kind);
no path means the current directory. Each file is formatted with the `format` options of the nearest
`tessera.json` at or above its directory (defaults when there is none). `--check` changes nothing and lists the files that would
change. Exit status: 0 done or already canonical, 1 `--check` found differences, 2 a file was refused,
unreadable or missing, or an option is unknown. Until ADR-077's unified `tessera` command lands the
binary is `tessera_fmt`, and it accepts `fmt` as an optional first argument so the documented
spelling works on either.

## 9. CI and ports

- `ctest -R source_format` formats every `.tsx` file under `samples/`, `packages/`,
  `tests/fixtures/`, `tools/reference/` and `designer/web/src/`. A file that parses must format and
  satisfy section 3; a file that does not parse must be refused as `TESSERA_FMT_SYNTAX`; a floor on
  the number formatted keeps the corpus from silently shrinking. A grammar form the printer cannot
  print therefore fails the build (the refusal is `TESSERA_FMT_UNSUPPORTED` or `..._VERIFY`).
- Not yet: `tessera_fmt --check` over the shipped samples and generated templates as its own CI step
  (the samples are not canonical yet; reformatting them moves line numbers that several goldens
  read) and the `format-on-save` studio setting.
- A port (ADR-058) runs `tessera fmt` once after translation so the project starts canonical.

## 10. Known limits of slice 1

- The CLI rewrites whole files; the edit list (section 11) is the editor-facing form.
- (`.css` and `tessera.json` are slice 3, sections 12 and 13.)
- Range formatting and recovery from recoverable errors (slice 4).
- Layout is greedy and local: ternaries and arithmetic do not break, there is no member-chain
  breaking, and assignments do not break after `=`.

## 11. Edit list and the studio (slice 2)

```cpp
FormatEditsResult format_edits(FileKind kind, std::string_view text, const FormatOptions& options = {});
std::vector<TextEdit> diff_text_edits(std::string_view before, std::string_view after);
std::string format_edits_json(FileKind kind, std::string_view text, const FormatOptions& options = {});
```

- `format_edits` is `format_source` followed by a diff against the input. A refusal carries the
  diagnostics and no edits, so a file is never half-edited.
- `TextEdit` is `{ offset, length, new_text }`: `offset` and `length` count **UTF-16 code units** of the
  input (a JavaScript string, CodeMirror), `new_text` is UTF-8. The list is in the coordinates of the
  input, ascending and non-overlapping, so it applies in any order; `designer/web/services/protocol.js`
  `toSequentialEdits` reverses it for `delta`, which applies one edit after another.
- **Minimal:** the diff runs over atoms (identifier runs, whitespace runs, single punctuation
  characters) after trimming the common prefix and suffix, with Myers' shortest edit script. A
  `'x'` to `"x"` change is two one-unit edits and an already canonical file is an empty list. A diff
  longer than 1500 edits falls back to one edit over the changed region (still correct, not minimal).
- Applying the list equals `format_source`'s text (tested on goldens, CRLF, a BOM, astral characters and
  random pairs). Line endings and the BOM follow section 2 because the diff is against the printed text.
- Wire form (`format_edits_json`, also `tessera_fmt --edits-json FILE` and the Wasm export
  `tessera_format_edits_json`): `{"edits":[{"offset","length","newText"}],"diagnostics":[{"code","message","line","column"}]}`.
- **Studio:** the `format` service ([studio-services-v1.md](studio-services-v1.md) section 8) answers
  `format` with those edits (`file` added). Format Document (`edit.format`, Shift+Alt+F) applies the list as
  one CodeMirror transaction: one undo step restores the text, the caret and selection map through the
  changes, and a text change while the worker was busy drops the answer. A refused file leaves the text
  untouched and the failure toast names `path:line:column code message`. The worker still sends `.tsx` only; the
  Wasm module also exports `tessera_format_edits_json_kind(kind, ...)` (0 tsx, 1 css, 2 tessera.json) for
  the studio to adopt.
- **Size:** `tessera_format.wasm` is 374 KB raw, 144 KB gzip-9 (plus a 3 KB loader), loaded lazily on the
  first format and not on the first-paint path, so `download_gzip_bytes` does not weigh it; a lazy-module
  entry in `tests/golden/perf/preview_budget.json` (like the mock transpiler's) is not added yet.
- **Format on save:** on by default (ADR-061 question 3). `file.save` formats the open `.tsx` file first;
  a refusal or a worker failure never blocks the save. The setting is the local preference
  `tessera.studio.formatOnSave` (`off` disables it; `designer/web/src/lib/format-prefs.ts`). A settings
  surface for the toggle is not in this slice.

## 12. CSS (slice 3)

The CSS printer is token based like the TSX one (section 1): it prints from the stylesheet engine's own
tokenizer (`src/ui/style/tokenizer.cpp`, comments included), so token boundaries are the engine's. It
never rewrites a token; it decides only whitespace, line breaks and the closing semicolon.

| Topic | Rule |
| --- | --- |
| Indent | `indentWidth` spaces per block level (default 2); `printWidth` is not used, CSS is never wrapped |
| Rule | selector list one selector per line (split at top-level commas, combinators and compound selectors never respaced), ` {`, one item per line, `}`; an empty rule is `a {}` |
| Declaration | `property: value;` on one line, a `;` always added after the last one, `;` that closes nothing dropped; property and value text are never re-cased or respaced beyond collapsing runs of whitespace |
| Multi-line value | a newline in a value outside parentheses stays a line break, continued one level deeper (grid areas, comma lists); inside parentheses whitespace collapses |
| Custom property | `--x: value;` the value is printed byte for byte (after trimming), including a balanced `{}` block |
| `@`-rule prelude | printed byte for byte between the name and the `{` or `;` (`@media (min-width:600px) {`) |
| Blocks | `@media`, `@supports`, `@layer`, `@keyframes`, `@font-face` and CSS nesting print their contents the same way, one level deeper |
| Blank lines | at most one, kept between items; none at the start of a block or the file |
| File | no content gives an empty file, otherwise exactly one trailing newline |

**Comments** are never lost and never moved to another construct: a comment on its own line is an item
of the block it stands in (at its indent, with the blank line before it kept); a comment after `;`, `{`
or `}` on the same line trails it; a comment inside a declaration or a prelude stays where it is,
between the same two tokens. A `/** ... */`-shaped block is re-indented, any other multi-line block is
printed byte for byte (as in section 4). Because every comment has a place, CSS has no
`TESSERA_FMT_COMMENT` refusal.

**Refusals** (`TESSERA_FMT_SYNTAX`, with line and column): an unclosed `{`, `(` or `[`, an unmatched `}`,
an unterminated string. The stylesheet parser itself is tolerant; the formatter is not, so an unbalanced
file is never rewritten. **Verification:** after printing, the output is tokenized again and must have the
same comments and the same sequence of non-whitespace tokens (a semicolon that closes nothing excepted),
else `TESSERA_FMT_VERIFY`. The tests additionally compare `parse_stylesheet` of input and output.

## 13. tessera.json (slice 3)

Printed as strict JSON: `indentWidth` spaces (default 2), `"key": value`, **key order and duplicate
keys preserved** (the compiler, not the formatter, owns the schema, so nothing is sorted), string and
number text byte for byte, one trailing newline. Objects and arrays that hold a non-scalar always break,
one member per line, with no trailing comma; an array of scalars stays on one line when it fits
`printWidth` (default 100, counted in code points), otherwise one item per line; `{}` and `[]` are
empty. JSON has no comments, so none is accepted: a `//` or `/* */`, a trailing comma, a single-quoted
string, a raw control character in a string or an empty file is refused as `TESSERA_FMT_SYNTAX`. The
output is parsed again and must have the same structure and text (`TESSERA_FMT_VERIFY`).

The two options come from the same file: `{"format": {"printWidth": 100, "indentWidth": 2}}`.
`printWidth` is 40..400 and `indentWidth` 1..8; a missing, mistyped or out-of-range value, or a file that
does not parse, leaves the default. Options never make a format fail. Existing files in the repository
keep compact objects (`"app": { "name": ... }`) until they are formatted; `fmt --check` over them is not
a CI step yet, for the same reason as the samples in section 9.

`ctest -R source_format` formats every `.css` file under `samples/`, `packages/`, `tests/`,
`tools/reference/`, `tools/wiki/src/` and `designer/web/src/` and every `tessera.json` there: each must
format, be idempotent and (CSS) mean the same to `parse_stylesheet` with no comment lost, with a floor on
the number found.
