# ADR-036: Own JSX/TSX front end and a React-like (not React-identical) model

- Status: Accepted
- Date: 2026-11-08
- Related (number: subject): ADR-007: neutral IR between JSX and C++ code generation; ADR-016:
  stylesheet engine; ADR-024: no JS/React runtime in production; ADR-025: error boundaries.
- Terms: **front end** = lexer + parser + analyzer that turns source text into an AST and then the
  IR. **IR** = the intermediate representation defined in `docs/specs/ir-v1.md`. **Subset** = the
  restricted part of JSX/TSX that Tessera accepts (to be defined in `docs/specs/jsx-subset-v1.md`,
  which does not exist yet). **Live view** = the preview in the browser designer, see ADR-037.

## Context

ADR-007 deferred the parser choice (SWC, oxc, or tree-sitter) to Big Step C. Those parsers are
general-purpose JavaScript/TypeScript front ends: they bring a Node or Rust toolchain, a much
larger language surface than we accept, and an AST we must immediately re-validate and reject
most of. Tessera already limits the language to a small, explicit subset and rejects the rest
loudly (AGENTS.md, section "Non-negotiables", item 8: support a small, explicit hooks subset and
reject unsupported patterns). The target is also not "run React in a browser": the
compiled UI is native C++ with its own renderer, layout, and stylesheet engine, so exact
parity with React plus a browser is neither achievable nor promised.

## Decision

1. **Own front end.** Tessera ships its own lexer/parser for a defined JSX/TSX subset
   (`src/compiler/analyzer/`), producing the normalized module AST and then the IR (ADR-007).
   SWC, oxc, tree-sitter, and Node.js are not build dependencies. The parser is C++, static,
   deterministic, and diagnoses with stable codes and source spans. The IR stays independent
   of the AST.
2. **React-like, not React-identical.** The authoring model follows React/JSX conventions
   (function components, props, the hooks `useState`, `useEffect`, `useRef` and
   `useAnimatedValue`, JSX composition, `style` and `className`) so React developers are productive. The compiled UI is
   "UI-like": it aims to look and behave like the web version for the supported subset, but
   there is no guarantee of pixel or behavioral equivalence with React plus a browser. The
   stylesheet compatibility matrix and component specs are the contract, not React/browser
   behavior. Differences are documented, not hidden.
3. **Built-in components and hooks.** The framework library provides built-in components
   (`docs/design/component-library.md`) and a registry of built-in hooks: the four hooks named
   in item 2 plus library hooks (planned examples: theme, breakpoint, navigation, view
   transitions; none is specified yet). Built-in hooks are declared in
   a manifest with typed signatures, so the analyzer validates them like core hooks.
   Hooks written by app developers (custom hooks), React context, suspense, and portals are
   rejected in V1 by the Tier 1 rules in `docs/design/compiler.md` section 4.
4. **Live view.** Authoring is edit-and-see. The Wasm designer is a statically compiled,
   self-contained engine that contains this front end and an IR evaluator: it parses UI source,
   lowers to IR, and renders immediately, with state preserved where safe (ADR-037). The live
   view and codegen consume the same IR, so preview and native cannot diverge on structure.
   This replaces the earlier assumption of a browser React runtime.
5. **Expression subset.** Expressions in JSX and handlers are limited to a documented,
   statically analyzable subset (literals, props/state reads, operators, member access, calls
   to declared functions/built-ins, ternary/`&&`, `map` over typed lists). Anything else is a
   hard error with a source span and a suggested fix.
6. The no-production-runtime rule is unchanged: no JS/React runtime, VDOM, or scripting
   engine ships in an app (ADR-024).

## Consequences

- No Node/Rust/JS toolchain for builds; fewer third-party licenses and faster, hermetic CI.
- We own parser correctness, error recovery, and TSX syntax coverage; a fuzzing and corpus
  suite is required. The cost is bounded by the small subset.
- Better diagnostics because the language is ours (precise "unsupported in Tessera" messages).
- Existing React code will not compile unchanged; documentation must state the subset and the
  fidelity goal clearly to avoid "100% React" expectations.
- Editor tooling (types for JSX props, autocomplete) must come from generated `.d.ts` files (as
  for custom elements) rather than the TypeScript compiler.
- This is an exception to AGENTS.md non-negotiable 2 ("Buy before build": do not re-implement
  solved infrastructure) for one component, the parser. It is justified because the
  subset is small, the surface must be controlled, and no existing parser removes the need for
  our own validation layer.

## Alternatives

- SWC and oxc (Rust JavaScript/TypeScript parsers): rejected (Node/Rust toolchain, full-language surface, parser-coupled risk).
- tree-sitter JSX/TSX grammars: rejected as the primary parser (untyped CST, grammar/subset
  mismatch); could be considered for editor syntax highlighting only (no decision made).
- Run real React in the live view: rejected (diverges from native behavior, ADR-024 intent).
- Promise full React + browser parity: rejected (unachievable, hides real differences).

## Follow-up

- Specify the TSX subset grammar and expression subset (`docs/specs/jsx-subset-v1.md`).
- Specify the built-in hook manifest and a written statement of which React and browser
  behaviors are and are not reproduced.
- Parser corpus, fuzz, diagnostics-golden, and determinism tests.
- Live-view execution model is decided in ADR-037 (self-contained Wasm engine).
