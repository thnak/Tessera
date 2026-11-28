## Samples: real author CSS for Demo and Counter, new Grid / Motion / Lists samples (#607, partial)

### What changed
- `samples/compiler-demo` (Demo) and `samples/parity-demo` (Counter) import their own sheet (`demo.css` with `@layer`, tokens, hover/active/focus-visible, transition, `@media`; `counter.css`), so the Studio no longer shows the fallback sheet for them.
- Pinned gates updated in the same landing: `codegen_cli_tests` copies `demo.css` beside every copy of `Demo.tsx`; `preview_parity_tests` lowers `Counter.tsx` with `counter.css`; `preview_budget_probe.mjs` stages sibling `import "./x.css"` sheets next to the entry (a missing one fails loudly); `wasm_ir_parity.py` refuses a sample whose imported sheet is missing (both sides would otherwise agree on the same diagnostic) plus a unit test; CMake DEPENDS on the sheets; `preview_budget.json` source note.
- New samples with README and tessera.json: `samples/grid` (grid areas, `@container`, `@media`), `samples/motion` (`@keyframes`, `transition`, reduced motion), `samples/lists` (keyed `.map` over a list state cell, conditional child). Browser twins `tools/parity/{grid,motion,lists}.html`.
- `docs/reference/samples.md` maps the checklist to these samples.

### Verification
- Linux box (GCC, preset dev): `codegen_cli`, `preview_parity` pass; all five samples compile with the CLI.
- Wasm (emsdk): `wasm_ir_parity.py` BYTE_IDENTICAL for IR and C++ including `counter_stylesheets.hpp`; the budget probe runs clean with the sheet staged.
- `python -B scripts/gates.py --quiet` 15/15; clang-format 22.1.3 on changed C++; `scripts/tests/test_wasm_ir_parity.py` 7 tests OK.

### What is left (#1018)
Browser-twin comparison runs (ua_audit/page.mjs) and twins for Demo/Counter, goldens regenerated from lavapipe, Gallery/Dash/Form checklist pass, components with props, springs (#603), typed input (#601). Lists limits found: a loop item inside an event handler and a top-level array constant are `TESSERA_IR_UNRESOLVED`.

Refs #607, #1018
