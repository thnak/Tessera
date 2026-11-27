# React port corpus measurement (spike #660)

Status: measurement, 2026-11-27, on main at `582d840f`. Evidence for
[ADR-066](../../decisions/ADR-066-react-core-additions-for-v1.md) (item 6, order of work) and
[`react-core-coverage.md`](../reference/react-core-coverage.md). It changes no front-end behavior.

## What was measured

- **Corpus:** `tests/fixtures/corpus/react-port/`, 43 small components, each written in this repository
  (MIT) to mirror common React and TypeScript spellings: `useState`/`useEffect`/`useMemo`/`useCallback`/
  `useRef`/`useReducer`/`useContext`/`useId`, a custom hook, arrow and `React.FC` components, `forwardRef`,
  `memo`, class error boundary, portal, `lazy`/`Suspense`, controlled inputs, a select, checkbox, lists with
  keys, early return, `switch`, `for`, optional chaining, object state with spread, inline styles, images,
  keyboard and mouse handlers. No third-party code was downloaded or copied.
- **Independence:** this is a different corpus from the 52-file golden corpus
  (`tests/fixtures/corpus/react/`, pinned by the `corpus_measure` test), which was written from the coverage
  table. These files were written from the idioms of everyday React and typed the way a TypeScript author
  types (`number`, `React.ReactNode`, `as const`), not the way the Tessera subset wants.
- **Tool:** [`scripts/corpus_react_port.py`](../../scripts/corpus_react_port.py) runs the existing
  `tessera_corpus_measure` (`tools/corpus`) with `--verbose` and tabulates it. Front end = parse plus the
  Tier-1 rules (every error counted). Pipeline = the whole single-file path, first blocking diagnostic.
  It is a measurement, not a gate: not in `gates.py`, not a CTest, because the share moves with every
  front-end change and needs a built binary.
- **Two runs.** `TESSERA_JSX_TYPE_NUMBER` (bare `number`) is a deliberate rejection decided by the owner
  ([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 5). It sits early, so it hides what a file needs
  next. Run A is the corpus as written. Run B (`--number-as-int`) measures a copy with the type name
  `number` rewritten to `int`, so the table of what comes after is readable. Run B is a sensitivity
  analysis, not a claim that `number` is accepted.

## Recompute

```text
cmake --preset vs2026
cmake --build build/vs2026 --config Release --target tessera_corpus_measure
python -B scripts/corpus_react_port.py                  # run A
python -B scripts/corpus_react_port.py --number-as-int  # run B
python -B scripts/corpus_react_port.py --json           # machine-readable, for the per-step diff
```

On Linux use `--preset dev`; the script finds `build/dev/tools/corpus/tessera_corpus_measure`
(`--binary PATH` overrides). Re-run after each ADR-066 step and record the two shares in the ADR step
evidence. The script prints an **anomalies** block (tool exit code, corpus files missing from the report,
files rejected with no diagnostic code) so a crash or a silent rejection is never counted as an ordinary
rejection.

## Result

| Run | Front end accepts | Pipeline accepts |
|---|---|---|
| A: as written | 15 of 43 (34.9%) | 2 of 43 (4.7%): `list-index-key`, `toggle-boolean` |
| B: `number` as `int` | 22 of 43 (51.2%) | 4 of 43 (9.3%): adds `todo-list`, `use-reducer-counter` |

**Anomalies: none.** The tool exited 0, every file was reported, no file crashed the front end, and no file
was rejected without a diagnostic code. Every rejection carries a code.

### Run A: front-end rejection by code

Occurrences, files containing the code, files for which it is the first error.

| Code | Occ. | Files | First |
|---|---|---|---|
| `TESSERA_JSX_TYPE_NUMBER` | 13 | 11 | 11 |
| `TESSERA_JSX_PARSE_UNEXPECTED_TOKEN` | 9 | 9 | 5 |
| `TESSERA_JSX_SUBSET_STATEMENT` | 3 | 3 | 2 |
| `TESSERA_JSX_PARSE_MISSING_SEMICOLON` | 3 | 3 | 3 |
| `TESSERA_JSX_SUBSET_MUTATION` | 3 | 3 | 2 |
| `TESSERA_JSX_SUBSET_FUNCTION_VALUE` | 2 | 2 | 2 |
| `TESSERA_JSX_SUBSET_ARRAY_METHOD` | 2 | 2 | 1 |
| `TESSERA_REACT_CONTEXT_FORM` | 2 | 1 | 1 |
| `TESSERA_REACT_HOOK_DEP`, `TESSERA_REACT_UNKNOWN_HOOK`, `TESSERA_JSX_SUBSET_IMPORT` | 1 each | 1 each | 0, 0, 1 |

`TYPE_NUMBER` is 11 of the 21 rejected files (26% of the corpus) and the first error in all 11. The two
hook codes appear only after `as const` broke the parse of a custom hook (a follow-on, not a cause).

### Run B: what remains with `number` cleared

Front end, 21 rejected files, first blocking cause each (the diagnostic message names the form):

| Code | Files | Form that triggers it |
|---|---|---|
| `PARSE_UNEXPECTED_TOKEN` | 6 | qualified type name `React.ReactNode`, `React.FC<P>` (3); `...` rest in a destructuring pattern and `[...rows]` array spread (2); `class` after `export` (1) |
| `PARSE_MISSING_SEMICOLON` | 3 | `as const` on a returned tuple (2); generic call `forwardRef<T, P>(...)` (1) |
| `SUBSET_STATEMENT` | 3 | `if` early return, `switch`, `for` in a component body |
| `SUBSET_MUTATION` | 3 | `let` in an effect, `document.title = x`, `ref.current = x` |
| `SUBSET_FUNCTION_VALUE` | 2 | an inner `function submit` in a component; `memo(function Row(...))` |
| `SUBSET_ARRAY_METHOD` | 2 | `.includes`, `.reduce` (only `.map`, `.filter`, `.slice` of ADR-066 item 3 are lowered) |
| `REACT_CONTEXT_FORM` | 1 | `createContext("light")` without an explicit type argument |
| `SUBSET_IMPORT` | 1 | dynamic `import("./Chart")` (`lazy`) |

Pipeline, 18 of the 22 front-end-accepted files fail later (first blocking code):

| Code | Files | Form that triggers it |
|---|---|---|
| `IR_UNSUPPORTED` | 6 | `{cond && <em/>}` inside `<Text>`; `setN(prev => prev + 1)` updater; `useState(initial)` with a prop-typed initial and no type argument; Fragment as the returned root; `src={u}` (literal only); `Math.max`/`Math.min` |
| `IR_UNKNOWN_PROP` | 4 | `onMouseEnter`, style `background`, `htmlFor`, `type="password"` on `TextInput` |
| one each | 8 | `CHILDREN_NOT_ALLOWED` (`<input>` inside `<label>`), `FOCUSABLE_NO_ROLE` (`onKeyDown` on a `div`), `IR_LITERAL` (a record update whose field is `e.target.value`), `IR_MALFORMED` (inline object type `{ city?: string }`), `IR_SLOT` (`children: ReactNode`), `SUBSET_HTML_TAG` (`<select>`), `IR_UNRESOLVED` (a module-scope `const TABS = [...]`), `IR_BINDING` (an effect with no `bind::`) |

## Reading it against ADR-066

**The model holds on the forms the ADR names.** Of the work items 1 to 5, the corpus exercises:
arrow components, `memo`, `useId`, `useMemo`/`useCallback`, `useReducer` (accepted once `number` is
`int`: `use-reducer-counter` compiles end to end) and `useContext`. The failures are exact spellings
around those forms, not a missing item.

**The order changes in one way, and the data is thin.** The ADR's remaining items are small in this
corpus: `useContext` static is 1 file (and it fails on the missing type argument before the provider
tree matters), list expressions 2 files, string methods 0. The frequent rejections are a different
set, mostly type-grammar and pipeline gaps that no ADR-066 row names:

1. **Type grammar of ordinary TypeScript (`number`, qualified names, `as const`, `...`).** 11 + 3 + 2 + 2 = 18
   of 43 files (42%) hit one of them first. `number` is a decided rejection and stays one; the hint is
   already in the message, so the cost is the porting edit, not the compiler. Qualified type names
   (`React.ReactNode`, `React.FC`) and `as const` are cheap parser acceptances (S) and are the
   largest *accept-able* group. This is what ADR-066 item 6 step 1 (parser and type-grammar gaps) was
   for; its list (helper, destructured local, `readonly`, JSDoc) is done, and this corpus finds the
   next layer. Recommendation: add it as a step 1b before the remaining item 6 work.
2. **Function values inside a component (updater, inner `function`, `memo(function ...)`):** 3 files.
   `setN(prev => prev + 1)` is the single most common React state idiom and is `IR_UNSUPPORTED` today.
3. **Component-body control flow (`if` early return, `switch`, `for`):** 3 files. The coverage table
   says "block-body `if` in render: no" and handler control flow landed, so this is a decided no that
   real code hits often; the corpus counts the cost, it does not argue the decision.
4. **Schema and lowering rows (`IR_UNKNOWN_PROP`, `IR_UNSUPPORTED`):** 10 of the 18 pipeline failures.
   These are not React forms (see the coverage table's "measured" rows) and are a larger share than any
   item still open in ADR-066.

Item 6's own order (cheap acceptances, custom hooks, handler flow, list/string, reducer/context) is
unchanged for what is still unlanded: reducer/context are the smallest groups in the data, so they stay
last. What changes is that **the next unlanded step by data is type-grammar acceptance (1b), then
function-value forms**, not item 5 or 6.

## Rejections not in react-core-coverage.md

Added to the coverage table by this note: qualified type names, `as const`, rest/array spread (`...` in a
pattern or an array literal), `createContext` without a type argument, `useState(init)` with a non-literal
initial and no type argument, functional state updaters, named function expressions as call arguments,
`Math.max`/`Math.min`, `.includes`/`.reduce`, component-body `if`/`switch`/`for`, dynamic `import()`,
a module-scope `const` array, an inline anonymous record type, `<select>`, and `children` typed as
`ReactNode`.

## Limits of this measurement

- 43 files, written by the author of the measurement from memory of common idioms. It is not a sample of
  published projects, and the counts are small: a table row of 1 or 2 files is a sighting, not a
  frequency. It is independent of the coverage table, not of the author's habits.
- Several files fail on more than one form. Run B reports the first cause only; a file blocked early
  can hide later forms, so the accepted share after fixing one row is at most the table suggests,
  not at least.
- The pipeline stage reports one blocking code per file by convention
  ([`compiler-diagnostics-v1.md`](../specs/compiler-diagnostics-v1.md) section 7), so it is not an
  occurrence table.
- A second, larger corpus of permissively licensed third-party components, kept out of the repository
  and recorded only as counts, remains the way to test whether these frequencies hold.
