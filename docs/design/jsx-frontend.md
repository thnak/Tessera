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
| 2 | AST, pure-expression parser (spec section 8), JSX element parser (section 10.1 to 10.4), subset rejections that are purely syntactic, AST dump | done |
| 3 | Module level: imports, type aliases, constants, component declarations, hook statements (sections 4 to 7), recovery per top-level item (section 14) | next |
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

Codes implemented so far:

- lexing, `TESSERA_JSX_LEX_*`: `INVALID_UTF8`, `LONE_CR`, `INVALID_CHARACTER`, `INVALID_NUMBER`,
  `INVALID_ESCAPE`, `UNTERMINATED_STRING`, `UNTERMINATED_TEMPLATE`, `UNTERMINATED_COMMENT`,
  `INVALID_TEXT_CHARACTER`;
- parsing, `TESSERA_JSX_PARSE_*`: `UNEXPECTED_TOKEN`, `UNCLOSED_ELEMENT`, `MISMATCHED_TAG`,
  `UNKNOWN_ENTITY`, `EMPTY_EXPRESSION`;
- subset rejections from spec section 12 that are visible in syntax alone,
  `TESSERA_JSX_SUBSET_*`: `HTML_TAG`, `DYNAMIC_TAG`, `LOOSE_EQUALITY`, `OPERATOR`, `MUTATION`,
  `MEMBER` (indexing), `CLASS`, `STATEMENT`, `ASYNC`, `FUNCTION_VALUE`, `IMPORT`, `TYPES`,
  `LITERAL` (computed or spread object keys), `UNKNOWN_PROP` (only for attribute names that are
  not identifiers), `DOM` (`dangerouslySetInnerHTML`);
- limits, `TESSERA_JSX_LIMIT_*`: `FILE_SIZE`, `TOKENS`, `DEPTH`, `DIAGNOSTICS`.

Per spec section 13.1 the full list is generated from the source, not copied here. Rows of
section 12 that need names, types or component context (`CALL`, `STYLE_DYNAMIC`, hook rules,
`TYPE_*`) belong to the analyzer slices, not the parser.

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

## Parser and AST

Entry points (`tessera/compiler/jsx/parser.hpp`): `parse_jsx_element(source, limits)` parses
exactly one element or fragment, `parse_expression(source, limits)` exactly one expression. Both
return `ParseResult<Node>`: the tree, every comment with its span, and the diagnostics.
`dump()` renders a deterministic S-expression used by the tests.

- **One AST node type per family** (`Expr`, `JsxNode`, in `ast.hpp`): a kind enum plus the
  fields that kind uses. No RTTI, no visitor machinery. The header is not part of the IR
  or codegen include path.
- **Error model.** A structural error (unexpected token, unclosed element, a limit) is fatal:
  the parse stops, `root` is null, and exactly one diagnostic is added (a lexer limit adds no
  parse noise after it). A subset violation (`<div>`, `==`, `x = 1`, an unknown entity, a
  mismatched closing tag) is reported and parsing continues, so independent errors are all found
  (spec 13.3); the tree is returned but `ok()` is false and it must not be lowered.
- **Operators.** Binary operators use precedence climbing with the section 8.1 levels.
  Operators the subset rejects occupy a precedence slot so their expression still parses.
- **Arrows.** `x => e`, `(a, b) => e` and block bodies of `;`-separated expressions
  (section 9.2). `(` is ambiguous, so the parser tries an arrow parameter list and, if it is not
  one, rewinds tokens, comments and diagnostics and parses a parenthesized expression.
- **JSX.** Text follows the standard whitespace rule and entity decoding of section 10.4;
  entity errors point at the `&name;` bytes. `{}` and `{/* c */}` are empty containers.
  Attributes are flag, string, `{expr}` or `{...spread}`; `aria-*` is the only hyphenated name
  family accepted.
- **Limits.** `max_nesting_depth` bounds parser recursion and also the height of every AST node.
  Heights are computed as nodes are built, so a 100000-term `a + b + ...` is a diagnostic
  and destroying any AST is bounded recursion.
- **Spans.** A node's span covers exactly its tokens, including parentheses around an
  expression. Tests slice the source with every span they check.

Tests (all deterministic, no clock or random ids): `jsx_lexer`, `jsx_expression`, `jsx_parser`,
and `jsx_adversarial` (deep nesting of every construct, size/token/diagnostic limits, unterminated
constructs, invalid UTF-8, NUL, every prefix and one-byte mutation of a sample, fixed-seed random
bytes; each must yield a tree or an error). Fuzzing against a corpus with a real fuzzer and the
diagnostics goldens of spec section 17 are still to do.

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
5. `{}` and `{/* comment */}` as a child are accepted as empty containers (standard JSX); the
   spec grammar has no empty `ChildExpr`.
6. A closing tag that does not match its opening tag is an error but not fatal: the parser treats
   it as closing the open element and continues.
7. Whitespace rule follows Babel exactly, including tabs becoming spaces.
8. Statement and declaration words in expression position (`if`, `class`, `function`, ...) are
   fatal; operator words (`typeof`, `in`, `await`) are reported and parsing continues.
