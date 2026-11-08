# ADR-007: A neutral IR between JSX and C++ codegen

- Status: Accepted (the "no error boundaries" [V1](../docs/reference/glossary.md) limit is superseded by [ADR-025](ADR-025-error-boundaries.md) (error boundaries))
- Date: 2026-11-07
- Related: [ADR-008](ADR-008-protected-regions-and-manifest.md) (protected regions and binding manifest), [ADR-025](ADR-025-error-boundaries.md)

## Context

The compiler has two tiers:

1. React/JSX analyzer (hooks rules, purity, valid syntax)
2. Framework rules engine (bindings, scene-node mapping)

If C++ codegen reads the parser AST directly, every parser change, target change, or tooling
change becomes a codegen change. We also want to inspect and snapshot the lowered UI without
running the generator.

## Decision

1. Define a **neutral, versioned IR (intermediate representation)** in `src/compiler/ir/`. Codegen consumes only the IR.
2. The IR describes:
   - module and imports
   - component name, parameters, slots/children
   - state hooks (`useState`), effects (`useEffect`), refs (`useRef`),
     animated values (`useAnimatedValue`)
   - binding identifiers from `// Note: bind::<id>`
   - node tree: `View`, `Text`, `Button`, `Image`, `Path`, `Scroll`, etc.
   - style properties (layout + visual), events, conditionals, static loops
   - text content
3. Tier 1 validates React semantics and rejects unsupported constructs **before** lowering.
4. Tier 2 lowers AST -> IR and resolves `bind::` annotations to binding ids.
5. Codegen maps IR nodes to C++ scene-graph construction and lifecycle stubs.
6. The IR is **serializable to JSON** for debugging, snapshot tests, and other tools (none are specified yet).
7. **V1 IR limits** (reject loudly):
   - no custom hooks
   - no context/suspense (error boundaries are now supported by [ADR-025](ADR-025-error-boundaries.md))
   - no dynamic component types
   - conditionals and loops must be statically analyzable; dynamic lists map to an explicit
     runtime list model
8. IR schema changes require a version bump and a migration note.

## Consequences

### Positive

- Parser, IR, and codegen evolve independently.
- IR snapshots make compiler changes reviewable without reading generated C++.
- A second target, such as a Wasm (WebAssembly) interpreter (not scheduled in this ADR), can consume the same IR.

### Negative / risks

- An extra layer to design and maintain.
- The IR can become a lowest-common-denominator if it tries to model all of React.
- Versioning and migrations add process overhead.

## Alternatives considered

- **Codegen directly from Babel/SWC AST.** Rejected: brittle, parser-coupled, hard to test.
- **Use React's reconciler in production.** Rejected: production must be pure C++ ([AGENTS.md](../AGENTS.md), non-negotiable 7, "No React runtime in production").
- **Adopt a generic agent-UI schema (e.g., A2UI).** Rejected: not expressive enough for hooks,
  effects, refs, and layout; A2UI is a runtime protocol, not a compile-time IR.
- **Generate a bytecode instead of C++.** Rejected for V1: loses native optimization and
  debugger-friendly output.

## Follow-up

- Write [`docs/specs/ir-v1.md`](../docs/specs/ir-v1.md) with a JSON schema and examples.
- Add IR snapshot tests to the compiler test suite.
- Parser: decided in [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own JSX front end and React-like model) (own front end; SWC/oxc/tree-sitter not used). The IR must not depend on it.
