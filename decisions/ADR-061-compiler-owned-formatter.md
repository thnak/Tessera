# ADR-061: The compiler owns formatting: one canonical printer, `tessera fmt`

- Status: Accepted (2026-11-19, owner). The owner asked for a proposal (decision D8 of the
  studio-as-IDE survey) and accepted it with all four proposals as written (see "Owner answers").
- Date: 2026-11-19
- Related: [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end),
  [ADR-024](ADR-024-native-first-boundaries.md) (no JS runtime in production),
  [ADR-060](ADR-060-studio-services-in-web-workers.md) (the format worker),
  [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md) (ported projects),
  [`designer-protocol-v1.md`](../docs/specs/designer-protocol-v1.md) section 10 (`source.patch`),
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md),
  [`docs/guides/code-style.md`](../docs/guides/code-style.md) (C++ only; this ADR is about TSX, CSS
  and project files)

## Context

The studio needs format-on-save and a format command, the CLI and CI need `fmt --check`, and the
canvas-to-source path (`source.patch`) writes edits into the author's file. The source files are
`.tsx` in Tessera's own dialect (ADR-036: `bind::`, the typed subset, `tsr-` classes, ADR-058
declarations), `.css`, and `tessera.json`.

The lexer already records every comment with its span in a side list
(`src/compiler/analyzer/.../lexer.hpp`, `token.hpp`), so the information a printer needs is
available; what does not exist is the rule that attaches a comment to a node, and the printer.

## Decision

### 1. The compiler ships the formatter

A library function `format_source(file kind, text, options)` returns an edit list or a refusal, in
the analyzer/lowering layer next to the parser it depends on, and is exposed three ways: the CLI
`tessera fmt [--check] [paths]`, the studio `format` worker (ADR-060), and the MCP server. One
printer means the studio, CI, an agent and a human get the same bytes.

### 2. Properties the printer must have (each is a test)

1. **Idempotent:** `format(format(x)) == format(x)`.
2. **Meaning-preserving:** parsing the output yields the same AST as the input, ignoring trivia.
3. **Comments are never lost.** The lexer's comment list is attached by rule: a comment on its own
   line leads the next node; a comment on the same line trails the previous token; a comment inside
   an empty construct is dangling. If a comment cannot be placed by those rules, the formatter
   **refuses** the file with a diagnostic naming the comment; it never drops or moves one silently.
4. **Deterministic:** no dependence on locale, clock, platform or iteration order.
5. **Refuses what it cannot parse.** A file with an unrecoverable syntax error is not formatted and
   the diagnostics are reported; a file with recoverable errors is formatted only in the regions that
   parsed (range formatting, step 2).

### 3. The result is an edit list, not a replacement

The printer computes the formatted text, then returns a minimal list of `TextEdit`s against the input
(a diff on tokens). This keeps the editor's undo history, the caret, the selection, the diagnostic
spans and the canvas-to-source mapping valid across a format, and it composes with `source.patch`
versions (a format is just another edit against a base version).

### 4. Style: one opinionated style, almost no options

The style is fixed by the compiler; accepted (Question 2): 2-space indent, print
width 100, double quotes, trailing commas where a construct breaks across lines, one attribute per
line when a JSX element breaks, `className` and `style` keep author order. The only project options,
under `format` in `tessera.json`, are `printWidth` and `indentWidth`; there is no per-rule switch.
CSS is printed one declaration per line, custom-property values and `@`-rule preludes untouched;
`tessera.json` is printed as 2-space JSON with key order preserved.

### 5. Where it runs

- **Studio:** a format command, and an optional format-on-save setting (Question 3).
- **CLI:** `tessera fmt` rewrites files; `--check` exits non-zero with the list of files that differ.
- **CI:** the repository's samples and the generated templates are checked with `--check`.
- **Ports (ADR-058):** a port runs `tessera fmt` once after the translation so the project starts
  canonical.

### 6. Slices

1. Printer for `.tsx` (whole file) with the three properties above, golden and idempotence tests over
   the samples and the conformance corpus, plus the CLI.
2. Edit-list output, the format worker and the studio command.
3. CSS and `tessera.json`.
4. Range formatting for files with recoverable errors; format-on-type is not planned.

## Consequences

- One definition of the code style across the studio, CI, MCP and the CLI; the diff produced by a
  canvas commit and a format are the same kind of edit.
- The printer must track the grammar: a new syntax form is not done until the printer prints it, and
  the conformance corpus fails the build otherwise.
- The comment attachment rules are a permanent contract; a refusal is preferred over a wrong guess.
- Authors cannot configure style details. That is the point, and it is the cost.

## Alternatives

- **Prettier in a worker.** Rejected: it does not parse `bind::`, the typed subset or the `tsr-`
  conventions without a plugin we would have to maintain against our own grammar; the native CLI and
  CI cannot run it without a JS runtime (ADR-024); it gives two sources of truth for the code style
  and cannot return a minimal edit list tied to our spans. It remains acceptable as a stopgap for
  CSS in the studio until slice 3.
- **No formatter, rely on the editor's indent.** Rejected: ported projects (ADR-058) and generated
  code need a canonical form, and `source.patch` diffs stay noisy without one.
- **Format on commit through `source.patch` only.** Rejected: it formats only what the canvas touched
  and gives no format command.

## Owner answers (2026-11-19)

1. **Compiler-owned printer** rather than Prettier: accepted.
2. **Style** (2 spaces, width 100, double quotes, trailing commas, one attribute per line when a JSX
   element breaks): accepted without change.
3. **Format-on-save in the studio: on by default**, with a setting to turn it off.
4. **An unplaceable comment refuses the file** (never drop, never guess): accepted.

## Follow-up

- Issues for slices 1-4, the conformance-corpus gate, `fmt --check` in CI, and a spec
  (`docs/specs/source-format-v1.md`) holding the comment rules and the style table.
- Mention `tessera fmt` in the port guide (ADR-058): noted in ADR-058's follow-up.
- Slice 1 (#576) is implemented: `format_source` in `src/compiler/format/`, the `tessera_fmt` CLI in
  `tools/fmt/`, and the spec above. The edit-list output, CSS and `tessera.json`, range formatting and
  `fmt --check` over the shipped samples are still open.
