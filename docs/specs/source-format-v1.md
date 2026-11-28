# Source format v1: the compiler-owned formatter

- Status: slice 1 implemented (`.tsx`, whole file, CLI). Slices 2 to 4 of
  [ADR-061](../../decisions/ADR-061-compiler-owned-formatter.md) (edit list, CSS and `tessera.json`,
  range formatting) are not in this version and are listed in section 10.
- Normative for: what `format_source` and `tessera fmt` guarantee, the comment attachment rules and
  the style table. The grammar being printed is
  [`jsx-subset-v1.md`](jsx-subset-v1.md) and [`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md).
- Code: `src/compiler/format/` (library `tessera_source_format`), `tools/fmt/` (the CLI),
  tests in `tests/unit/source_format_*.cpp`.

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

- `FileKind::Tsx` is the only kind in slice 1.
- `FormatOptions`: `print_width` (default 100) and `indent_width` (default 2). These are the two
  options ADR-061 allows; reading them from `format` in `tessera.json` arrives with slice 3.
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

A path is a file or a directory (walked for `.tsx`, skipping `node_modules` and dot directories);
no path means the current directory. `--check` changes nothing and lists the files that would
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

- Whole-file replacement only; the edit list of ADR-061 section 3 is slice 2.
- `.css` and `tessera.json` are not formatted (slice 3); `format.printWidth` and
  `format.indentWidth` are not read from `tessera.json` yet.
- Range formatting and recovery from recoverable errors (slice 4).
- Layout is greedy and local: ternaries and arithmetic do not break, there is no member-chain
  breaking, and assignments do not break after `=`.
