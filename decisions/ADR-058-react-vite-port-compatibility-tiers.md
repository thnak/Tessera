# ADR-058: Porting a React + Vite project: four compatibility classes, and a typing budget

- Status: Proposed (2026-11-19). The direction (port with small, mostly type-level edits) is the
  owner's; the classes, the typing rules and the order of work below need the owner's answers in
  "Questions for the owner" before they become Accepted.
- Date: 2026-11-19
- Related: [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end, React-like not
  React-identical), [ADR-039](ADR-039-lowercase-html-tags-accepted.md) (lowercase tags),
  [ADR-024](ADR-024-native-first-boundaries.md) (native-first boundaries; the no-JS-runtime rule
  that ADR-036 item 6 cites),
  [ADR-051](ADR-051-ir-record-types-and-record-values.md) (record types),
  [ADR-050](ADR-050/README.md) (app framework and services),
  [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md) (the feature-by-feature table
  this ADR extends), [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md),
  [`project-config-v1.md`](../docs/specs/project-config-v1.md),
  [`package-manifest-v1.md`](../docs/specs/package-manifest-v1.md),
  [`navigation-v1.md`](../docs/specs/navigation-v1.md)
- Terms: **port** = take an existing React + Vite + TypeScript project and make it compile as a
  Tessera project. **Subset** = the TSX Tessera accepts. **Shim package** = a package that keeps a
  popular npm package's import specifier and API surface but is implemented natively (section 2).

## Context

The project goal is that a React + Vite application ports with *small* edits, and that the edits are
mostly in typing, because Tessera is statically typed all the way to C++ while TypeScript lets a
React author leave much implicit. Today the port is blocked less by typing than by everything around
the component code. A survey of what a typical React + Vite project carries, against what the repo
accepts today (`react-compat.md`, `jsx-subset-v1.md`, `appfw`, ADR-036/039/051):

| Layer | What a Vite project typically has | Tessera today |
|---|---|---|
| Build | `vite.config.ts`, `index.html`, `public/`, `import.meta.env`, tsconfig `paths`, `?raw`/`?url`/glob imports, HMR | `tessera.json` (entry, `packages`, assets, build); live edit through the Wasm designer; no `import.meta.env`, no aliases, no glob imports |
| Language | TypeScript: `interface`, `enum`, generics, utility types, intersections, `?.`, `??`, `if`/`let`/`for` in handlers | `type` aliases, string-literal unions, `T \| null`, records (ADR-051). Rejected: `interface`, `enum`, user generics, intersections, `as`, `any`, `?.`, `??`, statements in handlers |
| React core | `useState`, `useEffect`, `useRef` | Supported. `useMemo`/`useCallback` lower in safe forms |
| | `useReducer`, `useContext`, `useId`, `useLayoutEffect`, `memo`, `lazy`, `Suspense`, portals, `forwardRef` | Not supported (`TESSERA_REACT_UNSUPPORTED_API`, member tags rejected, ...) |
| | Custom hooks | Supported: expanded at each call site ([ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md)) |
| Routing | react-router, TanStack Router | C++ `RouteTable`, `NavController`, deep links, restore exist (`tessera_appfw_navigation`); no TSX surface |
| Client state | Redux Toolkit, Zustand, Jotai, Context | `Observable<T>` in `appfw`; no TSX surface |
| Server data | fetch, axios, TanStack Query, SWR, WebSocket, GraphQL clients | C++ net client with fakes; no TSX surface |
| Forms | react-hook-form, formik, zod/yup | Library `Form`/`Field` and a forms controller; no schema library |
| Styling | CSS, CSS modules, Sass, PostCSS, Tailwind, styled-components/emotion | CSS and `*.module.css` through the stylesheet engine; the rest has no path |
| UI kits | MUI, Chakra, Ant, Radix/shadcn | `tessera-ui` library (stage 1 is 4 of 42 components, #527) |
| i18n | i18next, react-intl | C++ i18n service; no TSX surface |
| Animation | framer-motion, react-spring | Animation runtime, springs, keyframes, `useAnimatedValue` |
| Icons, SVG | lucide-react, react-icons, SVGR | Icon component planned (#530); vector paths (ADR-057) |
| Charts | recharts, d3, chart.js | Planned in the library farm plan |
| Utilities | lodash, date-fns, dayjs, clsx, uuid, immer | No path: bare imports other than `react`/`tessera` and configured package roots are errors |
| Quality | Vitest, Testing Library, Playwright, Storybook, ESLint | Native tests, the studio and goldens; #454 (app test support) is open |

Two facts bound every answer. There is no JS runtime in an app (ADR-024), so no npm package ever
*runs*; and `node_modules` cannot be compiled, because the transitive JavaScript it pulls in cannot
be bounded to the subset.

## Decision

### 1. Four classes. Every item of the survey lands in exactly one

- **A. Source-compatible.** Written in TSX/CSS exactly as in React and compiled natively. Components,
  props, `useState`/`useEffect`/`useRef`, JSX control flow, CSS and CSS modules, lowercase tags.
  Growing this class is the main lever (section 3).
- **B. Same API, native implementation (shim packages).** The import specifier and a documented API
  subset are kept, the implementation is native. A shim is an ordinary package mapped through
  `tessera.json` `packages` (`tessera-package.json`, `package-manifest-v1.md`) and declared by typed
  signatures the analyzer reads, like the built-in hook manifest (ADR-036 item 3). Candidates, each
  its own spec: a router (`react-router-dom` subset over `NavController`), a store (`zustand`-shaped
  over `Observable`), server state (`@tanstack/react-query`-shaped over the net client),
  `i18next`/`react-i18next` subset over the i18n service, `clsx`/`classnames` folded at compile time,
  an icon package (`lucide-react` names over the emitted icon set), a motion subset over the animation
  runtime. **A shim never emulates silently**: each ships a coverage table, and a call outside it is
  a diagnostic naming the supported form.
- **C. Build-time translation.** Vite concepts that exist only to build a web page become compile-time
  inputs: `vite.config.ts` fields to `tessera.json`; `import.meta.env.X` to constants folded at
  compile time from a declared `env` section (no runtime lookup); `public/` and asset imports to the
  asset manifest; tsconfig `paths` to a `tessera.json` alias table; HMR to the designer's live edit.
  Tailwind, Sass and PostCSS are not run by Tessera (no Node toolchain, ADR-036): the project's own
  build runs them, and the generated `.css` is accepted as an ordinary stylesheet if its properties are
  in [`stylesheet-property-status-v1.md`](../docs/specs/stylesheet-property-status-v1.md).
- **D. Not ported, with a named replacement.** DOM-dependent or runtime-JS code has no meaning here:
  `document`/`window`, `localStorage` (use the Storage service), DOM portals, `dangerouslySetInnerHTML`,
  iframes, CSS-in-JS runtimes (styled-components, emotion), DOM/SVG chart libraries before the
  library's charts exist, MUI/Chakra/Ant themselves (use `tessera-ui`). Every D item is a
  diagnostic that names its replacement, never a silent drop.

### 2. A port checker is part of the decision

A tool, `tessera port-check <vite project>`, reads `package.json` and the source and prints, per
dependency and per construct, its class (A/B/C/D) and the first blocking edit. It reuses the front
end's diagnostics and costs no runtime. It makes the goal measurable (counts per class, a list of
blockers) and keeps this ADR's table from going stale. It is the first deliverable (section 5).

### 3. The typing budget: what a port may be asked to change, and what Tessera absorbs

The target is that a port edits *types and declarations*, not logic. Proposed rules, in the order they
should land; each is additive to the IR (`irVersion` stays 1, `ir-v1.md` section 12):

1. **Absorbed by the compiler (no edit).** `interface X {..}` is accepted as a `type` alias; a
   string `enum` and `as const` objects as string-literal unions; `Partial`/`Pick`/`Omit`/`Required`
   and intersections of records with disjoint members are expanded at compile time; `?.` and `??` lower
   to null-checked conditionals on `T | null`; `import type` and type-only exports are free.
2. **Two ways to type a number, no third.** Owner decision: `number` is NOT accepted, not even where
   inference could decide. State is typed by inference (`useState(0)`, an integer literal with only
   integer operations is `int`, otherwise `float`) or by an explicit Tessera type hint
   (`useState<int>(0)`); `useState<number>(0)` is `TESSERA_JSX_TYPE_NUMBER` with the hint "write `int`
   or `float`". The rejection is loud on every surface (compiler CLI, scene preview CLIs on stderr,
   Studio diagnostics). **For port authors:** translate each `number` to `int` or `float`, or drop the
   hint and let the initializer decide. `react-compat.md` already calls `useState<int>` unavoidable.
3. **Declared by the author (the small edit).** `any`/`unknown` become a concrete type or a record;
   an untyped `useState([])` gets `useState<Item[]>([])`; object-shaped props get a named `type`;
   `Record<string, T>`/`Map` are not accepted until a map type exists (deferred).
4. **Hooks and logic.** Custom hooks are accepted by *inlining at compile time*, which preserves the
   hook-order rules (it is a function of the call site); `useContext`/`createContext` lower to the
   existing IR `Environment` with a typed default and a compile-time provider tree; `useReducer` lowers
   to a state cell plus a handler per action type. Handlers accept `if`, `const` and early `return`
   as IR actions; anything else is the escape hatch below.
5. **Escape hatch, typed.** Logic that does not fit stays in the developer C++ class through the
   existing `bind::` regions ([ADR-040](ADR-040-generated-base-and-developer-class.md)): the generated
   base declares the member's signature from the TS types, so the boundary is checked on both sides.

### 4. No npm source is ever compiled

`node_modules` is never read. Pure helper functions the *project* writes (`utils/format.ts`) may be
compiled by the same front end when they fit the expression subset; that needs its own ADR and is not
decided here. A package that is class B is replaced by its shim; a package that is neither is a class
D diagnostic. This keeps the "no JS toolchain, hermetic CI" property of ADR-036.

### 5. Order of work

1. `tessera port-check` and the class table in `react-compat.md` (cheap, measures everything after).
2. Typing rules 1-3 (section 3): pure front-end work, no runtime, biggest port win per line.
3. The TSX surface of what already exists in C++: router (the shim above), `useService` (#177),
   store and i18n shims. These need only specs and lowering, not new engines.
4. Rule 4 (custom hooks, context, reducer) and the asset/env/alias build-time pieces (class C).
5. Server-state and motion shims; project helper modules; maps.

Each step is its own issue family under milestone V1-M5; none is started by this ADR.

## Consequences

- A port has a written contract: a checker says where it stands, and every blocker has a named
  replacement. "React-like, not React-identical" (ADR-036) stays true; this makes the gap itemized.
- The cost is shim maintenance: each shim is an API we promise to keep compatible. The coverage-table
  rule and the "never emulate silently" rule bound that promise to what is tested.
- Typing rule 1 grows the subset's *syntax* (`interface`, `enum`, `?.`) without growing the IR, which
  keeps preview and native in lockstep (ADR-036 item 4). Rule 2 can reject a program a React author
  thinks is fine; the diagnostic must always carry the fix.
- Compiling hooks by inlining makes generated code larger for heavy hook use; measure it before rule 4
  is Accepted.
- It does not change ADR-024 or ADR-036: no JS runtime, no Node toolchain, own front end.

## Alternatives

- **Embed a JS engine and run the real React and npm packages.** Rejected: breaks ADR-024 and the
  startup, memory and determinism budgets; hides the type boundary.
- **Transpile `node_modules` to C++.** Rejected: unbounded transitive JavaScript, dynamic features
  (`eval`, prototypes, getters) the static model cannot express, and a licensing audit per package.
- **Support only a hand-written Tessera dialect (no port goal).** Rejected: contradicts the owner's
  goal and wastes the existing React knowledge the project is meant to leverage.
- **Make the type system gradual (`any` allowed).** Rejected: the generated C++ and the typed
  `bind::` boundary need concrete types; `any` would become a runtime type tag.

## Questions for the owner

1. Are the four classes the right cut, in particular class B (shim packages with a coverage table)?
2. Typing rule 2: DECIDED by the owner: bare `number` stays rejected; types are inferred or Tessera hints.
3. Typing rule 4: accept custom hooks by compile-time inlining, and `useContext` as `Environment`?
4. Should `tessera port-check` be built first, before any language change?
5. Tailwind/Sass: is "run by the project's own build, `.css` accepted" the policy, or should Tessera
   document a recommended Node-side step?

## Follow-up

- Add the class column and the port-checker spec to `docs/specs/jsx-intrinsics-v1/react-compat.md`.
- Open issues for steps 1-3 of section 5 under V1-M5; a router-in-TSX design note is the first one.
- When accepted, update `jsx-subset-v1.md` sections 5, 6 and 9 for each typing rule as it lands.
