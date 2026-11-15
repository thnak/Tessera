# Compiler diagnostics document v1

Status: proposed. Normative for `tools/codegen/` (the `--diagnostics-json` output and the browser
module) and for anything that consumes it. Companion to [`jsx-subset-v1.md`](jsx-subset-v1.md)
section 13 (the diagnostics themselves), [`ir-v1.md`](ir-v1.md) section 9 (IR validation) and
[`designer-protocol-v1.md`](designer-protocol-v1.md) section 3 (the wire shapes this reuses).

Terms: **diagnostic** is one reported problem (code, severity, message, span); **span** is a byte
range in a UTF-8 file with a resolved line/column pair; **document** is the JSON value this spec
defines. "V1" is the first-release scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8).

## 1. Why this artifact exists

The native CLI has always reported diagnostics, but only as *text*: the driver formats each one at
the moment it is reported, so the span is gone by the time anything else could consume it. The
design-time preview (issue #165, [`reference-equantic-ui.md`](../design/reference-equantic-ui.md)
section 7.4 slice P1) has to list diagnostics **with spans** in a browser, and its acceptance is that
those diagnostics match the native compiler's for the same input.

Both halves need one machine-readable form, so this is a versioned artifact rather than a private
output format: the page, CI and the eventual MCP `diagnostics` tool
([ADR-045](../../decisions/ADR-045-browser-hosted-studio.md) item 6) all read it.

## 2. Document

```json
{
  "diagnostics": [
    {
      "code": "TESSERA_IR_UNSUPPORTED",
      "message": "`<Nope>` is neither an intrinsic element nor a component declared or imported in this file",
      "severity": "error",
      "span": {
        "file": "ui/Counter.tsx",
        "offset": 44,
        "length": 6,
        "line": 3,
        "column": 11,
        "endLine": 3,
        "endColumn": 17
      },
      "suggestion": "declare or import the component, or fix the tag"
    }
  ],
  "diagnosticsVersion": 1,
  "file": "ui/Counter.tsx"
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `diagnosticsVersion` | integer | yes | Must be `1`. Section 6 |
| `file` | string | yes | The primary input of the run, POSIX separators; `""` when the run had no input |
| `diagnostics` | Diagnostic[] | yes | Every diagnostic of the run, in report order. Always present, `[]` when there are none |

An empty `diagnostics` array means the input compiled without diagnostics; it is a **success**
document, not a missing one. A consumer must never treat "no diagnostics" and "no document" alike: a
run that could not produce a document failed (section 5).

## 3. Diagnostic

| Field | Type | Required | Meaning |
|---|---|---|---|
| `code` | string | yes | A stable `TESSERA_*` code ([`error-model-v1.md`](error-model-v1.md) section 3). `""` only for a message the CLI could not classify |
| `severity` | string | yes | `"error"` or `"warning"`. The spelling of [`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md) section 13.1, never the enum name |
| `message` | string | yes | The user-facing text, already bounded (section 4) |
| `span` | SourceSpan | no | Omitted when the diagnostic has no source location |
| `suggestion` | string | no | Omitted when empty |
| `related` | RelatedSpan[] | no | Omitted when empty; each entry is `{ "span": SourceSpan, "message": string }` |

`span` is the **SourceSpan of [`designer-protocol-v1.md`](designer-protocol-v1.md) section 3.1**, which
is the same span as [`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md) section 13.2: project-relative
`file` with POSIX separators, byte `offset` and `length` into the UTF-8 source, and 1-based
`line`/`column` where a column counts Unicode code points, with `endLine`/`endColumn` exclusive.

A **missing** `span` is meaningful: a setup or usage error (a bad flag, an unreadable file) has no
source position, and placing it at `1:1` would underline the first character of a file it never
examined. Consumers must render such a diagnostic without a location, not with a guessed one.

## 4. Bounded text

A document never grows with the input through its messages. The guarantee is the front end's, not a
second limit applied here: `jsx::DiagnosticSink` truncates every `message`, `suggestion` and related
message at `jsx::kMaxDiagnosticTextBytes` (256 bytes) on a UTF-8 code-point boundary, and enforces
`Limits::max_diagnostics` (100) with one `TESSERA_JSX_LIMIT_DIAGNOSTICS` entry in place of the rest
([`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md) section 15). The document writer must not re-truncate:
a second cut would be a second rule, and the two would drift.

## 5. Production and ordering

One writer produces the document: `write_diagnostics_json()` in
`tools/codegen/diagnostics_json.cpp`. The native CLI (`--diagnostics-json`) and the browser module
(`tessera_diagnostics_json`) both call it, so there is no second assembly path to keep in step.

**The document contains the diagnostics the CLI reports.** That is a *prefix* of the pipeline: the
stages run parse → analyse → lower → validate, and the driver stops at the first stage that fails,
reporting that stage's first diagnostic. A run therefore normally holds exactly one entry, and an
empty array means no stage failed.

This is the CLI's existing and documented contract, not an artifact of this format: `--check` and
every script built on the CLI see the same first-error behaviour, and a diagnostics query does not
change it. It is also not a statement that the input has only one problem — the front end collects
all independent errors it can find in a file ([`jsx-subset-v1-jsx.md`](jsx-subset-v1-jsx.md)
section 13.3) and the validator reports every hard error in a module; the driver chooses to stop.
A complete-list query over that collected set is a separate, later slice and is tracked in
[`docs/reference/open-questions.md`](../reference/open-questions.md); it is not part of version 1.

* **Order** is report order: source order within a stage, under the pipeline's own rules.
* **Determinism.** The text is a pure function of the input and the invocation: no clock, no locale,
  no environment, no filesystem state beyond the files named. Keys are written in byte order and
  container members in report order, so two runs of the same input are byte-identical.
* **Failure.** A run that cannot write the document exits `2` (setup failure) and reports the
  failing path; it never leaves a partial document. The document is written even when the run itself
  failed with `1`, because the diagnostics are the answer the caller asked for.

## 6. Versioning

The artifact follows [`versioning-v1.md`](versioning-v1.md) section 2: `diagnosticsVersion` is `1`.

* Adding an optional field keeps the version.
* Changing a field's meaning, removing a field, or changing the default of a member bumps it, and the
  bump ships a compatibility note and a version check in the consumer.
* A consumer rejects an unknown `diagnosticsVersion` loudly instead of reinterpreting it.

## 7. Native and browser parity

P1's acceptance is that the page reports what the native compiler reports, so parity is a gate, not a
promise: `scripts/ci/wasm_diagnostics_parity.py` runs the host CLI and the Emscripten module over one
corpus and requires **byte-identical** documents. Byte identity, not "equivalent", because the
document is canonical (section 5): any difference is a real drift in which stage stops first, in a
message, or in a span.

The corpus must not be all-valid — a gate that passes when both sides report nothing proves nothing —
so each case declares a minimum diagnostic count and the gate fails a case that under-reports before
it compares bytes. The cases cover a clean file, a lexical error, a structural parse error, a
lowering rejection, a rules rejection, a span that is not on the first line, and a multi-byte UTF-8
line before the error (so the byte offset and the code-point column are both exercised).

The same agreement is proven locally without Emscripten by `codegen_diagnostics_json`
(`tests/unit/codegen_diagnostics_json_tests.cpp`), which runs the file-driven CLI driver and the
in-memory driver over the same corpus and compares their documents. That is what keeps the two
drivers in step: the Wasm module is the in-memory driver behind a C export, so the CI gate's remaining
job is to prove the export wraps it, not to discover a pipeline difference.

### 7.1 What the browser cannot report

The page has one in-memory buffer and no project, so two CLI behaviours are deliberately absent, and
both are reported the way the CLI reports the equivalent situation:

* **No id salt.** Nothing assigns `componentId`/`uiId`/`automationId`, so the validator's
  `TESSERA_IR_MISSING_ID` complaints for them are not actionable and are not reported — exactly the
  filter an unsalted CLI run applies.
* **No files.** A side-effect `import "./x.css"` has no text to compile, so lowering reports
  `TESSERA_IR_STYLESHEET_MISSING` at the import span, which is the CLI's diagnostic for a stylesheet
  that is not there. A relative or package import is an ordinary lowering diagnostic.

**The unsalted-run rule (normative).** An unsalted run defers `TESSERA_IR_MISSING_ID` for
`componentId`, `uiId` and `automationId`; the preview page runs unsalted, so it matches an unsalted
CLI run. Parity (section 7) is therefore stated against an unsalted native run, and a future slice
that gives the page a salt (P5 share links, [`reference-equantic-ui.md`](../design/reference-equantic-ui.md)
section 7.4) must pass that salt to *both* sides of the gate rather than changing what the page
reports on its own.

## 8. Command line

```text
tessera_codegen --diagnostics-json FILE [--namespace NAME] [--salt SALT] [--root DIR] FILE.tsx...
```

`--diagnostics-json` writes one document and generates no C++, so `--out` is not required; it is
mutually exclusive with `--emit-ir` and `--emit-library-cpp`. Exit codes follow the CLI's existing
contract: `0` with an empty array, `1` when the document lists at least one error-level diagnostic,
`2` for a setup failure.
