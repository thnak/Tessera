# JSX/TSX front end (lexer and parser)

Status: Big Step C, first slices implemented in `src/compiler/analyzer/` (library
`tessera_jsx_frontend`, namespace `tessera::compiler::jsx`). Decision: ADR-036
([`../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md`](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)).
Normative language: [`../specs/jsx-subset-v1.md`](../specs/jsx-subset-v1.md). Pipeline context:
[`compiler.md`](compiler.md) section 3. This note records how the code is organized and what is
and is not done; the spec stays the single source for grammar and diagnostic meaning.

The front end is portable C++ (no OS, thread, or filesystem calls). The caller reads the file and
passes the text, so the same library links into the native compiler and the Wasm designer.

## Slice plan

| Slice | Content | State |
|---|---|---|
| 1 | Spans, diagnostics, limits, UTF-8 checks, parser-driven tolerant lexer | done |
| 2 | AST, pure-expression parser (spec section 8), JSX element parser (section 10.1 to 10.4), subset rejections that are purely syntactic, AST dump | next |
| 3 | Module level: imports, type aliases, constants, component declarations, hook statements (sections 4 to 7), recovery per top-level item (section 14) | later |
| 4 | Handlers, `bind::` comment association, list-loop form, attribute rules (sections 9, 10.2, 10.5) | later |
| 5 | AST -> IR lowering in `src/compiler/ir/` (codegen still never reads the AST) | later |

Slices 1 and 2 only parse a standalone JSX element or a pure expression. There is no module
parser yet, so nothing consumes the `Comment` list for `bind::` annotations; the lexer keeps
every comment with its span for that purpose.

## Source positions

`Span` is a byte offset plus byte length. Tokens and AST nodes store only that. Line and column
(1-based, columns count code points, LF ends a line so CRLF is one break, a leading BOM is not a
column) are computed on demand by `SourceMap`, so a 1 MiB single-line file does not cost
quadratic time. `Diagnostic` carries a `ResolvedSpan` (offset, length, start and end location).

## Diagnostics

`DiagnosticSink` stores the first `max_diagnostics` entries, then one
`TESSERA_JSX_LIMIT_DIAGNOSTICS` entry, then only counts. `has_errors()` stays correct after
truncation. `format_diagnostic` renders `line:col: error[CODE]: message` with optional `help:` and
`note:` lines; that text is what the golden tests compare.

Codes implemented so far (lexing): `TESSERA_JSX_LEX_INVALID_UTF8`, `_LONE_CR`,
`_INVALID_CHARACTER`, `_INVALID_NUMBER`, `_INVALID_ESCAPE`, `_UNTERMINATED_STRING`,
`_UNTERMINATED_TEMPLATE`, `_UNTERMINATED_COMMENT`, `_INVALID_TEXT_CHARACTER`; limits:
`TESSERA_JSX_LIMIT_FILE_SIZE`, `_TOKENS`, `_DEPTH`, `_DIAGNOSTICS`. Per spec section 13.1 the full
list is generated from the source, not copied here.

## Lexer

The meaning of characters depends on position, so the parser drives the lexer with a mode per
token (`LexMode`):

- `Code`: expressions. Identifiers, reserved words (`Keyword`), numbers, strings, template
  parts, punctuators.
- `JsxTag`: after `<`. Names may contain `-`, there are no keywords (`class`, `type` are plain
  names), and `<`, `>`, `/` and `/>` are single tokens. `</>` therefore lexes as `<` then `/>`.
- `JsxChild`: between tags. Only `{`, `<` and raw `JsxText` exist; `//` is text, not a comment.
  A bare `}` or `>` in text is reported (`_INVALID_TEXT_CHARACTER`) and kept as text.

Guarantees: every `next()` consumes at least one byte or returns `Eof`; malformed input produces
a diagnostic and a token (recovery never fabricates tokens). Invalid UTF-8 and lone CR are found
by one pass in the constructor, independent of context. Operators the subset rejects (`==`, `??`,
`?.`, `**`, `++`, `+=` and so on) are lexed by maximal munch so the parser can give the specific
code from spec section 12. Templates are lexed in parts: the parser calls `continue_template`
after the `}` that closes a substitution.

## Decisions taken where the spec was silent

These are interpretations, listed so the owner can overrule them.

1. Attribute strings use the same escapes as other strings (spec 3.4), not HTML-style raw
   strings.
2. Keywords are only recognized in `Code` mode, and a keyword is accepted as a member name after
   `.` (`row.type`).
3. `.5` is `.` followed by `5` (spec 3.4 lists decimal integers and fractions with a leading
   digit only).
4. Parser nesting limit applies to AST height as well as recursion, so a chain of more than 128
   operands (`a + b + c + ...`) is `TESSERA_JSX_LIMIT_DEPTH`.
