# ADR-040: Generated base class plus developer-owned class instead of protected regions

- Status: Accepted (2026-11-11)
- Date: 2026-11-10
- Supersedes: [ADR-008](ADR-008-protected-regions-and-manifest.md) (protected regions, binding
  manifest, idempotent merging). The region merge path stays in the code only until the migration
  in Follow-up lands; do not extend it.
- Related: [ADR-007](ADR-007-compiler-ir.md) (compiler IR),
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end; native handlers and
  preview bodies), [ADR-037](ADR-037-self-contained-wasm-designer.md) (Wasm designer),
  [ADR-038](ADR-038-cpp23-baseline.md) (C++23 baseline), issue
  [#3](https://github.com/thnak/Tessera/issues/3).

## Context

ADR-008 keeps developer C++ inside `[DEVELOPER_LOGIC_START/END: bind::<id>]` regions of generated
files and re-inserts it on every regeneration with a text merger. The codegen slice in
[`compiler.md`](../docs/design/compiler.md) section 11 now emits these regions for native handlers
(`// Note: bind::<id>`, [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) section 9.3), effects
and effect cleanups. Using it end to end showed these costs:

1. **No single owner for the file that holds developer code.**
   [`developer-workflow.md`](../docs/guides/developer-workflow.md) section 3 says to commit the
   generated files; section 4 says the output directory is build output that stays out of version
   control, and that the team commits a seed (`tests/golden/codegen/demo/Demo.cpp`) that the build
   regenerates over. The seed is developer-owned yet mostly generated text, so it drifts outside
   its regions whenever codegen changes.
2. **Renames and removals become dead code, not errors.** A region only in the existing file is
   kept with a warning. Rename migration tooling is still an open question
   ([`compiler.md`](../docs/design/compiler.md) section 12).
3. **Regions hold bodies only.** A developer has no place for a member (`HttpClient http_`), an
   `#include`, or a private helper function.
4. **Tooling exists only to keep the merge safe:** marker comments as a generated-file ABI, the
   binding manifest as a merge input, the pinned clang-format as part of the merge contract, the
   idempotency invariant and outside-region drift in `--check`.
5. **Debugging** developer code inside regenerated files needs source maps (an open question in
   `compiler.md` section 12).
6. **Developer code reaches generated internals.** Region bodies write `state_count_` directly, so
   every generated field name is an implicit API and a setter cannot attach behavior (for example
   scheduling an update).

ADR-008 rejected "partial classes / inheritance hooks" because C++ has no partial classes and
hooks "still need a merge policy", and rejected "user-owned files with generated callbacks only"
because of wiring boilerplate. Both objections assume the developer file is regenerated or that
the developer writes the wiring. Neither is needed: the base class holds all wiring, and the
developer file is written once and never touched by the generator, so there is nothing to merge.

## Decision

1. **Two files per component, each with exactly one owner.**
   - Generated: `<Component>.g.hpp` / `<Component>.g.cpp` declare and define
     `<namespace>::gen::<Component>Base<Self>`. They are build output: regenerated wholesale, never
     edited, never committed, never merged.
   - Developer-owned: `<Component>.hpp` / `<Component>.cpp` declare
     `<namespace>::<Component> final : public gen::<Component>Base<<Component>>`. The generator
     never rewrites them.
2. **The base owns everything derived from IR.** State cells, refs, animated values, the node
   tree, `build`/`mount`/`update`/`unmount`, `apply_state`, child sets, event adapters and inline
   setter handlers ([`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) sections 9.1 and 9.2) are
   generated in full. Fields are private. Developer code uses generated protected accessors:
   `x()` and `set_x(value)` per state cell, `ref_<name>()` per ref, `<name>()` per prop, and the
   animated-value handle. Accessor names come from the TSX names, not from generated field names.
3. **Every `bind::<id>` becomes a required member of the developer class.**

   | TSX | Required member |
   |---|---|
   | native handler `bind::<id>` with typed params, return `R` | `R <id>(params)` |
   | `useEffect` with `bind::<id>` | `void <id>()` |
   | effect cleanup (`bind::<id>_cleanup`) | `void <id>_cleanup()` |

   Parameter and return types map from IR types exactly as codegen maps them today.
4. **Static dispatch (CRTP).** Private dispatch thunks in the base call
   `static_cast<Self&>(*this).<id>(...)`. No vtable, no RTTI, no allocation, inlinable. The
   developer class declares `friend` the base so handlers can stay private.
5. **Missing or mismatched members fail the C++ build with a TSX location.** For each bind id the
   base emits, inside a member function that is always instantiated (`build()`), a
   `static_assert` on a requires-expression whose message names the bind id, the expected
   signature and the TSX `file:line`. The check must not sit at class scope, because `Self` is an
   incomplete type when the base template is instantiated. Codegen also emits a
   `gen::<Component>Contract<Self>` concept for IDE tooling.
6. **Scaffold once, then report.** `tessera_codegen --scaffold` (and the first `tessera generate`
   for a component) writes the developer files only when they do not exist, with one stub per bind
   id. Afterwards the CLI never edits them: for a new bind id it reports
   `TESSERA_CG_MISSING_BINDING` with the stub as the suggested fix; for a removed bind id it warns
   that the member is now unused. `generated/bindings.json` stays as a report for review, not as a
   merge input.
7. **Setters own update behavior.** `set_x(value)` assigns the cell and raises the component's
   dirty state exactly as generated setters do today. Changing that to scheduling an update
   automatically is a separate decision (issue [#5](https://github.com/thnak/Tessera/issues/5));
   this ADR only makes it possible by routing every developer write through a setter.
8. **Version control.** Commit `.tsx` sources and developer files; keep the generated directory
   out of version control. `--check` compares generated output only; developer files cannot drift.
9. **Preview is unchanged.** The designer keeps running inline setter handlers and native-handler
   preview bodies from IR ([ADR-037](ADR-037-self-contained-wasm-designer.md)); only where the
   production C++ lives changes.

## Consequences

### Positive

- A renamed, removed or retyped binding is a compile error at the right place instead of an
  orphan region plus a warning; no rename migration tool is needed.
- The C++ text merger, marker ABI, merge idempotency invariant and the formatter-as-merge-contract
  dependency are retired from the codegen path. Determinism becomes simpler: generated output is a
  pure function of the IR and generator version.
- Developer members, includes and helpers have a normal home; breakpoints and IDE navigation work
  on ordinary C++ without source maps.
- Generated field names stop being API, so codegen can change storage freely behind accessors.
- Resolves the commit-or-not contradiction in `developer-workflow.md`.

### Negative / risks

- Developers write member signatures. Mitigated by the scaffold and the
  `TESSERA_CG_MISSING_BINDING` fix text; a TSX signature change needs a manual edit, which the
  build flags.
- Templates add some compile time and can produce long diagnostics. Mitigated by the targeted
  `static_assert` messages; one base instantiation per component keeps the cost bounded.
- The developer header includes the generated header, so a generated header change rebuilds the
  developer translation unit. This matches today's single-file behavior.
- Existing region-based code must be migrated (only `samples/compiler-demo` today).

## Alternatives considered

- **Keep ADR-008.** Rejected for the reasons in Context; the cost grows with every component and
  every developer.
- **Virtual interface (pure virtual hooks).** Simpler diagnostics, but adds a vtable per component
  and a missing override is only an error when the class is instantiated. Kept as a fallback if the
  CRTP diagnostics prove too poor on one of the supported compilers.
- **Runtime callback slots** (`on_submit([&](...) { ... })`, as in Slint). Rejected: a missing
  handler is found only at run time, and `std::function` storage allocates.
- **AST-level merging.** Rejected for the same reasons as in ADR-008.
- **Macros as the extension point.** Rejected for the same reasons as in ADR-008.

## Implementation status

Slice 1 (issue #157, 2026-11-13) landed the base emitter as an opt-in mode,
`codegen::generate(module, embeds, theme, EmitMode::GeneratedBase)`, writing one header-only
`<Component>.g.hpp` per component (`src/compiler/codegen/src/emit_base.cpp`). The legacy
`<Component>.hpp/.cpp` path and its goldens are unchanged. What exists:

- `gen::<Component>Base<Self>` as a CRTP template; every definition is an `inline` template member
  in the header; helpers live in `namespace <Component>_detail`; event and scheduler thunks are
  function templates over `Self`.
- One same-named dispatch thunk per `bind::<id>` (item 4), and a `static_assert` at the start of
  `build()` per binding that checks the developer class defines the member with a compatible
  signature (an inherited thunk does not count), with a `TESSERA_CG_MISSING_BINDING` message
  naming the bind id, the signature and the TSX `file:line` (item 5); plus the
  `gen::<Component>Contract<Self>` concept.
- Accessors named after the TSX: `x()` per state, `ref_<name>()` per ref, `<name>()` per prop and
  animated value (item 2). A name that collides with a generated member is a codegen error.
- Tested by `codegen_base_tests` (shape, golden `tests/golden/codegen/base/Sample.g.hpp`,
  determinism), `codegen_base_runtime_tests` (a developer class reaching the thunks),
  compile-fail tests for a missing member and a wrong signature, and a build-time compile of every
  emitter family (lists, virtual collections, vector icons, composition, size observation, forms)
  with explicit instantiation of every template member, on Windows (MSVC) and Linux (GCC).

Known limits of slice 1, closed by the later slices: generated fields and thunks are public, not
private with protected accessors (the event thunks cast their context to the base and need access);
the CLI does not expose the mode or write the developer files yet (#209); `compiler-demo` and the
merger still use the legacy path (#210); documents still describe protected regions (#211). A child
component in a parent is the child's developer class, so a parent's header includes
`<Child>.hpp`.

## Follow-up

- Codegen: emit `<Component>Base<Self>`, accessors/setters, dispatch thunks, contract concept and
  the per-binding `static_assert`; regenerate goldens in a separate reviewed change.
- CLI: `--scaffold`, `TESSERA_CG_MISSING_BINDING`, removed-binding warning.
- Migrate `samples/compiler-demo` and `tests/golden/codegen/demo/Demo.cpp`; then remove the C++
  region path from the merger and update its tests.
- On acceptance: mark ADR-008 `Superseded by ADR-040`, and update
  [`developer-workflow.md`](../docs/guides/developer-workflow.md),
  [`compiler.md`](../docs/design/compiler.md) sections 7, 8 and 11,
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) section 9.3, `CLAUDE.md` and `AGENTS.md`.
